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
 * gowl-module-broadcast.c - Input broadcasting for multiboxing (opt-in).
 *
 * Repeats keys -- and, where a rule asks, clicks -- typed into one
 * window of a TEAM into its other windows, as the same key or another
 * (see src/boxed/gowl-broadcast.h for teams and rules, and
 * docs/broadcast.org).  The ISBoxer / AutoHotkey-multibox idea, done in
 * the compositor, which is the one place on Linux that can put real
 * input into a window without focusing it.
 *
 * Loaded only when asked for -- `modules: broadcast: {enabled: true}' in
 * gowl's YAML, or (gowl-enable-module "broadcast") in cmacs -- and even
 * then it repeats nothing until switched ON with its toggle key
 * (Super+b by default), which shows a toast either way.  The panic key
 * (Super+Shift+b) switches it off and releases every key it is holding
 * anywhere.
 *
 * How a key travels:
 *
 *   filter_key (early, before every bind and shortcuts-inhibit):
 *       the toggle, panic and slot-focus keys are taken here, so they
 *       work while a fullscreen game inhibits shortcuts.  A press is
 *       routed here too, while broadcasting is on: a rule with `local:
 *       block' takes the key from the focused window, which can only be
 *       decided before it is forwarded.  The release of a key whose
 *       press never reached the focused window is taken here too.
 *   key_forwarded (after the key reached the focused window):
 *       the press -- or the release -- is repeated into the targets.
 *       For an X11 focused window it first waits until Xwayland has
 *       processed the local key, or moving X focus to a target could
 *       land the local key in the target; and releases go AFTER the
 *       local one because an X server has one key state for all its
 *       windows.  A release a bind claimed never arrives here; the next
 *       key event lets its targets go instead, so nothing stays held.
 *
 * Every press remembers exactly where it went (the `held' table) and
 * its release goes to exactly those windows, whatever has changed in
 * between -- a toggle, a reload, a rule edit.  Switching off, panic,
 * deactivation and shutdown release everything still held.
 *
 * Delivery itself is gowl_compositor_deliver_key(): a Wayland window
 * gets the key through a brief seat focus; an X11 window through a
 * confirmed X focus move (gowl-xwayland-sync.c).  A `delay-ms' rule's
 * deliveries go through a queue drained by a wl_event_loop timer --
 * never a GLib timeout, which under cmacs would run on Emacs's thread.
 *
 * IPC (replies "OK ...", "OK <json>" or "ERROR ..."):
 *
 *   broadcast-on / -off / -toggle      switch broadcasting
 *   broadcast-panic                    off, and release everything
 *   broadcast-status                   state, teams, slots, rules (JSON)
 *   broadcast-list                     teams and rules as YAML (JSON)
 *   broadcast-add-team YAML            add/replace a team (this session)
 *   broadcast-add-rule YAML            add/replace a rule (this session)
 *   broadcast-remove-team NAME         remove a team
 *   broadcast-remove-rule NAME         remove a rule
 *   broadcast-enable-rule NAME         put a rule in force
 *   broadcast-disable-rule NAME        take it out without removing it
 *   broadcast-clear                    forget runtime teams and rules
 *   broadcast-reload                   re-read the config's
 *   broadcast-assign TEAM SLOT [ID]    put window ID (default: focused)
 *                                      in a slot by hand
 *   broadcast-unassign TEAM SLOT       back to the slot's `match'
 *   broadcast-focus-slot [TEAM] SLOT   focus a slot's window
 *   broadcast-step-reset [RULE]        round-robin back to the start
 *   broadcast-strict on|off|toggle     one press, one window
 */

#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "gowl-broadcast"

#include <glib-object.h>
#include <gmodule.h>
#include <json-glib/json-glib.h>
#include <string.h>
#include <wayland-server-core.h>
#include <xkbcommon/xkbcommon.h>

#include "module/gowl-module.h"
#include "interfaces/gowl-input-observer.h"
#include "interfaces/gowl-ipc-handler.h"
#include "interfaces/gowl-startup-handler.h"
#include "interfaces/gowl-shutdown-handler.h"
#include "core/gowl-compositor.h"
#include "core/gowl-core-private.h"
#include "core/gowl-client.h"
#include "core/gowl-monitor.h"
#include "config/gowl-config.h"
#include "config/gowl-keybind.h"
#include "boxed/gowl-broadcast.h"

#include "gowl-broadcast-engine.h"

#define BC_PREFIX              "broadcast-"
#define BC_DEFAULT_TOGGLE      "Super+b"
#define BC_DEFAULT_PANIC       "Super+Shift+b"
#define BC_DEFAULT_MAX_TARGETS (16)
#define BC_HARD_MAX_TARGETS    (64)
#define BC_DEFAULT_TIMEOUT_MS  (80)

#define GOWL_TYPE_MODULE_BROADCAST (gowl_module_broadcast_get_type())
G_DECLARE_FINAL_TYPE(GowlModuleBroadcast, gowl_module_broadcast,
                     GOWL, MODULE_BROADCAST, GowlModule)

/* One delivered press, remembered so its release follows it. */
typedef struct {
	guint            client_id;
	guint32          code;       /* keycode, or button */
	gint             modifiers;
	GowlKeyDelivery  delivery;
	gdouble          x;          /* clicks only */
	gdouble          y;
} Held;

/* One delivery waiting for its delay. */
typedef struct {
	guint            client_id;
	guint32          keycode;
	gboolean         pressed;
	gint             modifiers;
	GowlKeyDelivery  delivery;
	gint64           due_us;
} Queued;

struct _GowlModuleBroadcast {
	GowlModule              parent_instance;

	GowlCompositor         *compositor;     /* weak */
	GowlConfig             *config;         /* ref: whose set is loaded */
	gulong                  reloaded_handler;

	GowlBroadcastEngine    *engine;
	gboolean                active;         /* module activated */
	gboolean                on;             /* broadcasting */

	/* settings */
	gboolean                start_on;
	gboolean                toast;
	gboolean                log;
	guint                   max_targets;
	guint                   timeout_ms;
	gchar                  *toggle_text;
	guint                   toggle_mods;
	guint                   toggle_sym;
	gchar                  *panic_text;
	guint                   panic_mods;
	guint                   panic_sym;

	/* keycode -> GArray<Held>: where each held press went */
	GHashTable             *held_keys;
	/* button -> GArray<Held> */
	GHashTable             *held_buttons;
	/* keycode -> GArray<GowlBroadcastAction>: routed in filter_key,
	 * delivered by key_forwarded */
	GHashTable             *pending;
	/* keycodes whose press this module took; their release is too */
	GHashTable             *swallowed;
	/* keycodes whose release is on its way to the focused window; the
	 * targets get theirs after it (key_forwarded), or at the next key
	 * event if a bind claimed it and it never arrived */
	GHashTable             *release_due;

	GQueue                 *queue;          /* element-type Queued* */
	struct wl_event_source *queue_timer;

	/* counters for status */
	guint64                 n_delivered;
	guint64                 n_failed;
	gchar                  *last_error;

	struct wl_listener      display_destroy;
};

static void bc_observer_init(GowlInputObserverInterface *iface);
static void bc_ipc_init(GowlIpcHandlerInterface *iface);
static void bc_startup_init(GowlStartupHandlerInterface *iface);
static void bc_shutdown_init(GowlShutdownHandlerInterface *iface);

G_DEFINE_TYPE_WITH_CODE(GowlModuleBroadcast, gowl_module_broadcast,
	GOWL_TYPE_MODULE,
	G_IMPLEMENT_INTERFACE(GOWL_TYPE_INPUT_OBSERVER, bc_observer_init)
	G_IMPLEMENT_INTERFACE(GOWL_TYPE_IPC_HANDLER, bc_ipc_init)
	G_IMPLEMENT_INTERFACE(GOWL_TYPE_STARTUP_HANDLER, bc_startup_init)
	G_IMPLEMENT_INTERFACE(GOWL_TYPE_SHUTDOWN_HANDLER, bc_shutdown_init))

/* ── Small helpers ───────────────────────────────────────────────────── */

static gint64
now_ms(void)
{
	return g_get_monotonic_time() / 1000;
}

/* A line for the journal: routine at info, everything when `log'. */
static void G_GNUC_PRINTF(2, 3)
bc_log(
	GowlModuleBroadcast *self,
	const gchar         *format,
	...
){
	g_autofree gchar *msg = NULL;
	va_list ap;

	va_start(ap, format);
	msg = g_strdup_vprintf(format, ap);
	va_end(ap);
	if (self->log)
		g_message("%s", msg);
	else
		g_info("%s", msg);
}

static void
bc_fail(
	GowlModuleBroadcast *self,
	const gchar         *what
){
	self->n_failed++;
	g_free(self->last_error);
	self->last_error = g_strdup(what);
	bc_log(self, "broadcast: %s", what);
}

/* The toast (layout-indicator draws it), a bar toast when the bar is
 * loaded, and the module event cmacs and IPC subscribers listen to. */
static void
bc_announce(
	GowlModuleBroadcast *self,
	const gchar         *toast,
	const gchar         *event
){
	if (self->compositor == NULL)
		return;
	if (self->toast && toast != NULL) {
		g_autofree gchar *line = NULL;
		g_autofree gchar *flat = NULL;

		flat = g_strdelimit(g_strdup(toast), "|\n", ' ');
		g_signal_emit_by_name(self->compositor, "toast-requested",
			gowl_compositor_get_selected_monitor(self->compositor), flat);
		line = g_strdup_printf("bar-notify %s|", flat);
		g_free(gowl_compositor_run_command(self->compositor, line));
	}
	if (event != NULL)
		gowl_compositor_emit_module_event(self->compositor, "broadcast",
		                                  "%s", event);
}

/* The live client with @id, or NULL: deliveries look a window up again
 * every time rather than keep a pointer that can dangle. */
static GowlClient *
client_by_id(
	GowlModuleBroadcast *self,
	guint                id
){
	GList *l;

	if (self->compositor == NULL || id == 0)
		return NULL;
	for (l = gowl_compositor_get_clients(self->compositor); l != NULL;
	     l = l->next)
		if (gowl_client_get_id(GOWL_CLIENT(l->data)) == id)
			return GOWL_CLIENT(l->data);
	return NULL;
}

/* A window's facts for the engine; strings borrowed from @c. */
static void
window_of(
	GowlClient          *c,
	GowlBroadcastWindow *w
){
	GowlMonitor *mon;

	memset(w, 0, sizeof *w);
	w->id = gowl_client_get_id(c);
	w->app_id = gowl_client_get_app_id(c);
	w->title = gowl_client_get_title(c);
	w->pid = (gint)gowl_client_get_pid(c);
	w->xwayland = gowl_client_get_xwayland(c);
	w->tags = gowl_client_get_tags(c);
	mon = (GowlMonitor *)gowl_client_get_monitor(c);
	w->monitor = mon != NULL ? gowl_monitor_get_name(mon) : NULL;
	w->floating = gowl_client_get_floating(c);
	w->fullscreen = gowl_client_get_fullscreen(c);
}

/* Re-binds windows to slots.  @gone, when not NULL, is a client being
 * removed that may still be on the list. */
static void
bc_assign(
	GowlModuleBroadcast *self,
	GowlClient          *gone
){
	g_autoptr(GArray) windows = NULL;
	GList *l;

	if (self->compositor == NULL)
		return;
	windows = g_array_new(FALSE, TRUE, sizeof(GowlBroadcastWindow));
	for (l = gowl_compositor_get_clients(self->compositor); l != NULL;
	     l = l->next) {
		GowlBroadcastWindow w;

		if (l->data == (gpointer)gone)
			continue;
		if (gowl_client_get_wlr_surface(GOWL_CLIENT(l->data)) == NULL)
			continue;
		window_of(GOWL_CLIENT(l->data), &w);
		g_array_append_val(windows, w);
	}
	gowl_broadcast_engine_assign(self->engine,
	                             (GowlBroadcastWindow *)windows->data,
	                             windows->len);
}

static guint
under_mouse_id(GowlModuleBroadcast *self)
{
	GowlClient *c;

	if (self->compositor == NULL || self->compositor->wlr_cursor == NULL)
		return 0;
	c = gowl_compositor_client_at(self->compositor,
	                              self->compositor->wlr_cursor->x,
	                              self->compositor->wlr_cursor->y);
	return c != NULL ? gowl_client_get_id(c) : 0;
}

/* ── Config sync ─────────────────────────────────────────────────────── */

static void bc_on_config_reloaded(GowlConfig *config, gpointer data);

/* Loads the set of the config the compositor uses now.  A reload may
 * replace the config object, so identities are compared; the ref on the
 * loaded one rules out a freed-and-reused address comparing equal. */
static void
bc_sync_config(GowlModuleBroadcast *self)
{
	GowlConfig *current;

	if (self->compositor == NULL)
		return;
	current = gowl_compositor_get_config(self->compositor);
	if (current == self->config)
		return;

	if (self->config != NULL) {
		if (self->reloaded_handler != 0)
			g_signal_handler_disconnect(self->config,
			                            self->reloaded_handler);
		self->reloaded_handler = 0;
		g_clear_object(&self->config);
	}
	if (current == NULL) {
		gowl_broadcast_engine_set_config(self->engine, NULL, NULL);
		return;
	}
	self->config = g_object_ref(current);
	self->reloaded_handler = g_signal_connect(current, "reloaded",
		G_CALLBACK(bc_on_config_reloaded), self);
	gowl_broadcast_engine_set_config(self->engine,
		gowl_config_get_broadcast_teams(current),
		gowl_config_get_broadcast_rules(current));
	bc_assign(self, NULL);
}

static void
bc_on_config_reloaded(
	GowlConfig *config,
	gpointer    data
){
	GowlModuleBroadcast *self = GOWL_MODULE_BROADCAST(data);

	gowl_broadcast_engine_set_config(self->engine,
		gowl_config_get_broadcast_teams(config),
		gowl_config_get_broadcast_rules(config));
	bc_assign(self, NULL);
	if (gowl_broadcast_engine_get_n_problems(self->engine) > 0)
		bc_log(self, "broadcast: %u rule(s) cannot run; see "
		       "broadcast-status",
		       gowl_broadcast_engine_get_n_problems(self->engine));
}

/* ── Delivery ────────────────────────────────────────────────────────── */

static gboolean bc_queue_tick(gpointer data);

/* One key event to one window, now. */
static gboolean
deliver_key_now(
	GowlModuleBroadcast *self,
	guint                client_id,
	guint32              keycode,
	gboolean             pressed,
	gint                 modifiers,
	GowlKeyDelivery      delivery
){
	GowlClient *c;

	c = client_by_id(self, client_id);
	if (c == NULL)
		return FALSE;
	if (!gowl_compositor_deliver_key(self->compositor, c, keycode, pressed,
	                                 modifiers, delivery)) {
		g_autofree gchar *what = g_strdup_printf(
			"key %u %s to window %u (%s) was not delivered", keycode,
			pressed ? "press" : "release", client_id,
			gowl_client_get_app_id(c) != NULL
			? gowl_client_get_app_id(c) : "?");

		bc_fail(self, what);
		return FALSE;
	}
	self->n_delivered++;
	return TRUE;
}

/* Arms the queue timer for the head of the queue. */
static void
queue_arm(GowlModuleBroadcast *self)
{
	Queued *head;
	gint64 wait_us;

	if (self->compositor == NULL)
		return;
	head = g_queue_peek_head(self->queue);
	if (head == NULL)
		return;
	if (self->queue_timer == NULL)
		self->queue_timer = wl_event_loop_add_timer(
			gowl_compositor_get_event_loop(self->compositor),
			(wl_event_loop_timer_func_t)bc_queue_tick, self);
	if (self->queue_timer == NULL)
		return;
	wait_us = head->due_us - g_get_monotonic_time();
	wl_event_source_timer_update(self->queue_timer,
		wait_us <= 0 ? 1 : (gint)((wait_us + 999) / 1000));
}

/* The queue timer: delivers everything due, in order, in one batch. */
static gboolean
bc_queue_tick(gpointer data)
{
	GowlModuleBroadcast *self = GOWL_MODULE_BROADCAST(data);
	gboolean batch = FALSE;
	Queued *q;

	while ((q = g_queue_peek_head(self->queue)) != NULL
	       && q->due_us <= g_get_monotonic_time() + 500) {
		g_queue_pop_head(self->queue);
		if (self->compositor != NULL) {
			if (!batch) {
				gowl_compositor_input_batch_begin(self->compositor);
				batch = TRUE;
			}
			deliver_key_now(self, q->client_id, q->keycode, q->pressed,
			                q->modifiers, q->delivery);
		}
		g_free(q);
	}
	if (batch)
		gowl_compositor_input_batch_end(self->compositor);
	queue_arm(self);
	return FALSE;
}

static void
queue_clear(GowlModuleBroadcast *self)
{
	g_queue_clear_full(self->queue, g_free);
	if (self->queue_timer != NULL)
		wl_event_source_timer_update(self->queue_timer, 0);
}

/* Delivers, or queues behind what is already queued so nothing
 * overtakes: a release may never arrive before its press. */
static void
deliver_or_queue(
	GowlModuleBroadcast *self,
	guint                client_id,
	guint32              keycode,
	gboolean             pressed,
	gint                 modifiers,
	GowlKeyDelivery      delivery,
	gint64               due_us
){
	if (g_queue_is_empty(self->queue) && due_us <= g_get_monotonic_time()) {
		deliver_key_now(self, client_id, keycode, pressed, modifiers,
		                delivery);
		return;
	} else {
		Queued *q = g_new0(Queued, 1);
		Queued *tail = g_queue_peek_tail(self->queue);

		q->client_id = client_id;
		q->keycode = keycode;
		q->pressed = pressed;
		q->modifiers = modifiers;
		q->delivery = delivery;
		q->due_us = tail != NULL ? MAX(due_us, tail->due_us) : due_us;
		g_queue_push_tail(self->queue, q);
		queue_arm(self);
	}
}

/* The keycode an action sends, and the modifiers to hold for it. */
static gboolean
action_key(
	GowlModuleBroadcast       *self,
	const GowlBroadcastAction *a,
	guint32                    typed_keycode,
	guint32                   *out_keycode,
	gint                      *out_modifiers
){
	gboolean shift = FALSE;
	guint32 kc;

	if (a->keysym == 0) {
		*out_keycode = typed_keycode;
		*out_modifiers = a->modifiers;
		return TRUE;
	}
	kc = gowl_compositor_keysym_to_keycode(self->compositor, a->keysym,
	                                       &shift);
	if (kc == 0)
		return FALSE;
	*out_keycode = kc;
	*out_modifiers = (a->modifiers < 0 ? 0 : a->modifiers)
	                 | (shift ? GOWL_KEY_MOD_SHIFT : 0);
	return TRUE;
}

/* Sends a routed PRESS to its targets and remembers each one under the
 * typed keycode, for the release. */
static void
deliver_press(
	GowlModuleBroadcast *self,
	guint32              typed_keycode,
	GArray              *actions
){
	GArray *held;
	gint64 due;
	guint i;
	guint n;

	if (actions == NULL || actions->len == 0 || self->compositor == NULL)
		return;

	held = g_hash_table_lookup(self->held_keys,
	                           GUINT_TO_POINTER(typed_keycode));
	if (held == NULL) {
		held = g_array_new(FALSE, TRUE, sizeof(Held));
		g_hash_table_insert(self->held_keys,
		                    GUINT_TO_POINTER(typed_keycode), held);
	}

	n = MIN(actions->len, self->max_targets);
	due = g_get_monotonic_time();
	gowl_compositor_input_batch_begin(self->compositor);
	for (i = 0; i < n; i++) {
		GowlBroadcastAction *a = &g_array_index(actions,
		                                        GowlBroadcastAction, i);
		Held h;
		guint32 kc;
		gint mods;

		if (!action_key(self, a, typed_keycode, &kc, &mods)) {
			g_autofree gchar *what = g_strdup_printf(
				"no key in the layout produces keysym 0x%x (rule %s)",
				a->keysym, gowl_broadcast_rule_get_name(a->rule));

			bc_fail(self, what);
			continue;
		}
		if (i > 0)
			due += (gint64)a->delay_ms * 1000;
		deliver_or_queue(self, a->client_id, kc, TRUE, mods, a->delivery,
		                 due);
		memset(&h, 0, sizeof h);
		h.client_id = a->client_id;
		h.code = kc;
		h.modifiers = mods;
		h.delivery = a->delivery;
		g_array_append_val(held, h);
		if (self->log)
			bc_log(self, "broadcast: key %u -> window %u (slot %s) "
			       "as %u", typed_keycode, a->client_id, a->slot, kc);
	}
	if (actions->len > n)
		bc_log(self, "broadcast: %u target(s) over max-targets %u "
		       "skipped", actions->len - n, self->max_targets);
	gowl_compositor_input_batch_end(self->compositor);
}

/* Sends the release of everything @typed_keycode's press went to. */
static void
deliver_release(
	GowlModuleBroadcast *self,
	guint32              typed_keycode
){
	GArray *held;
	guint i;

	held = g_hash_table_lookup(self->held_keys,
	                           GUINT_TO_POINTER(typed_keycode));
	if (held == NULL)
		return;
	if (self->compositor != NULL) {
		gowl_compositor_input_batch_begin(self->compositor);
		for (i = 0; i < held->len; i++) {
			Held *h = &g_array_index(held, Held, i);

			deliver_or_queue(self, h->client_id, h->code, FALSE,
			                 h->modifiers, h->delivery,
			                 g_get_monotonic_time());
		}
		gowl_compositor_input_batch_end(self->compositor);
	}
	g_hash_table_remove(self->held_keys, GUINT_TO_POINTER(typed_keycode));
}

/* Releases every key and button held anywhere; nothing queued survives. */
static void
release_all(GowlModuleBroadcast *self)
{
	g_autoptr(GList) keys = NULL;
	GHashTableIter iter;
	gpointer key;
	gpointer value;
	GList *l;

	g_hash_table_remove_all(self->pending);
	g_hash_table_remove_all(self->release_due);

	/* Queued presses never go out; their releases are below */
	if (!g_queue_is_empty(self->queue)) {
		GList *q;

		for (q = self->queue->head; q != NULL; q = q->next)
			if (((Queued *)q->data)->pressed)
				((Queued *)q->data)->due_us = 0;
	}

	keys = g_hash_table_get_keys(self->held_keys);
	for (l = keys; l != NULL; l = l->next)
		deliver_release(self, GPOINTER_TO_UINT(l->data));

	g_hash_table_iter_init(&iter, self->held_buttons);
	while (g_hash_table_iter_next(&iter, &key, &value)) {
		GArray *held = value;
		guint i;

		for (i = 0; i < held->len; i++) {
			Held *h = &g_array_index(held, Held, i);
			GowlClient *c = client_by_id(self, h->client_id);

			if (c != NULL)
				gowl_compositor_deliver_button(self->compositor, c,
				                               h->code, FALSE, h->x, h->y);
		}
	}
	g_hash_table_remove_all(self->held_buttons);
	/* Everything still queued is a release now; send it at once */
	if (!g_queue_is_empty(self->queue))
		bc_queue_tick(self);
}

/* ── Switching ───────────────────────────────────────────────────────── */

static void
set_on(
	GowlModuleBroadcast *self,
	gboolean             on,
	const gchar         *why
){
	g_autofree gchar *toast = NULL;
	g_autofree gchar *event = NULL;
	const gchar *team = NULL;
	const gchar *slot = NULL;
	GowlClient *sel;

	if (!on)
		release_all(self);
	self->on = on;
	gowl_broadcast_engine_reset_steps(self->engine, NULL);

	sel = self->compositor != NULL
	      ? gowl_compositor_get_focused_client(self->compositor) : NULL;
	if (sel != NULL)
		gowl_broadcast_engine_slot_of(self->engine,
		                              gowl_client_get_id(sel),
		                              &team, &slot);
	if (on && team != NULL)
		toast = g_strdup_printf("Broadcast ON — team %s, %u window(s)",
			team, gowl_broadcast_engine_count_bound(self->engine, team));
	else if (on)
		toast = g_strdup_printf("Broadcast ON — focused window is in no "
		                        "team (%s toggles)", self->toggle_text);
	else
		toast = g_strdup("Broadcast OFF");
	event = g_strdup_printf("%s %s", on ? "on" : "off",
	                        team != NULL ? team : "-");
	bc_log(self, "broadcast: %s (%s)", on ? "on" : "off", why);
	bc_announce(self, toast, event);
}

static void
do_panic(GowlModuleBroadcast *self)
{
	release_all(self);
	queue_clear(self);
	self->on = FALSE;
	gowl_broadcast_engine_reset_steps(self->engine, NULL);
	bc_log(self, "broadcast: panic");
	bc_announce(self, "Broadcast stopped — every key released",
	            "off panic");
}

/* Focuses a slot's window from anywhere (a slot's `focus-key'). */
static void
focus_slot(
	GowlModuleBroadcast *self,
	const gchar         *team,
	const gchar         *slot,
	guint                client_id
){
	GowlClient *c = client_by_id(self, client_id);

	if (c == NULL) {
		g_autofree gchar *t = g_strdup_printf("Slot %s of %s is empty",
		                                      slot, team);

		bc_announce(self, t, NULL);
		return;
	}
	gowl_compositor_show_client(self->compositor, c);
}

/* ── GowlInputObserver ───────────────────────────────────────────────── */

/* Is the key the bind @mods + @sym? */
static gboolean
key_is(
	const GowlInputObserverKey *key,
	guint                       mods,
	guint                       sym
){
	return sym != 0 && key->modifiers == mods
	       && (xkb_keysym_to_lower(key->base_keysym) == xkb_keysym_to_lower(sym)
	           || xkb_keysym_to_lower(key->keysym) == xkb_keysym_to_lower(sym));
}

static gboolean
bc_filter_key(
	GowlInputObserver          *observer,
	const GowlInputObserverKey *key
){
	GowlModuleBroadcast *self = GOWL_MODULE_BROADCAST(observer);
	gpointer kc = GUINT_TO_POINTER(key->keycode);

	if (!self->active || self->compositor == NULL)
		return FALSE;

	/* A release that was due and never reached key_forwarded (a bind
	 * claimed it): its targets let go now, so nothing stays held. */
	if (g_hash_table_size(self->release_due) > 0) {
		g_autoptr(GList) due = g_hash_table_get_keys(self->release_due);
		GList *l;

		g_hash_table_remove_all(self->release_due);
		for (l = due; l != NULL; l = l->next)
			deliver_release(self, GPOINTER_TO_UINT(l->data));
	}

	if (!key->pressed) {
		g_hash_table_remove(self->pending, kc);
		/* A key the focused window never saw: its targets let go
		 * now, and the release is consumed like the press was. */
		if (g_hash_table_remove(self->swallowed, kc)) {
			deliver_release(self, key->keycode);
			return TRUE;
		}
		/* Otherwise after the focused window's own release (see
		 * key_forwarded) -- an X server has one key state, and the
		 * local release must not find it already released. */
		if (g_hash_table_contains(self->held_keys, kc))
			g_hash_table_add(self->release_due, kc);
		return FALSE;
	}

	/* Our own keys, working under shortcuts-inhibit too */
	if (key_is(key, self->panic_mods, self->panic_sym)) {
		g_hash_table_add(self->swallowed, kc);
		do_panic(self);
		return TRUE;
	}
	if (key_is(key, self->toggle_mods, self->toggle_sym)) {
		g_hash_table_add(self->swallowed, kc);
		set_on(self, !self->on, "key");
		return TRUE;
	}
	{
		const gchar *team;
		const gchar *slot;
		guint id;

		if (gowl_broadcast_engine_focus_key(self->engine, key->modifiers,
		                                    key->base_keysym, &team, &slot,
		                                    &id)) {
			g_hash_table_add(self->swallowed, kc);
			focus_slot(self, team, slot, id);
			return TRUE;
		}
	}

	/* A press to route: synthetic input is never repeated (a KVM, a
	 * macro, our own replays would multiply) */
	g_hash_table_remove(self->pending, kc);
	if (self->on && !key->synthetic && key->focused != NULL) {
		g_autoptr(GArray) actions = NULL;
		GowlBroadcastWindow w;
		gboolean block = FALSE;

		window_of(GOWL_CLIENT(key->focused), &w);
		actions = g_array_new(FALSE, TRUE, sizeof(GowlBroadcastAction));
		if (gowl_broadcast_engine_route_key(self->engine, &w,
		        under_mouse_id(self), key->keysym, key->base_keysym,
		        key->modifiers, now_ms(), actions, &block)) {
			if (block) {
				/* Never reaches the focused window: deliver now */
				g_hash_table_add(self->swallowed, kc);
				deliver_press(self, key->keycode, actions);
				return TRUE;
			}
			if (actions->len > 0)
				g_hash_table_insert(self->pending, kc,
				                    g_steal_pointer(&actions));
		}
	}
	return FALSE;
}

static void
bc_key_forwarded(
	GowlInputObserver          *observer,
	const GowlInputObserverKey *key
){
	GowlModuleBroadcast *self = GOWL_MODULE_BROADCAST(observer);
	GArray *actions;
	gpointer kc = GUINT_TO_POINTER(key->keycode);

	if (self->compositor == NULL)
		return;
	if (!key->pressed) {
		if (!g_hash_table_remove(self->release_due, kc))
			return;
		if (key->focused != NULL
		    && gowl_client_get_xwayland(GOWL_CLIENT(key->focused))
		    && !gowl_compositor_await_x11_key(self->compositor,
		                                      key->keycode, FALSE))
			bc_fail(self, "the focused X11 window did not confirm "
			              "its own release");
		deliver_release(self, key->keycode);
		return;
	}
	if (!g_hash_table_steal_extended(self->pending, kc, NULL,
	                                 (gpointer *)&actions))
		return;

	/* The local key first: until Xwayland has processed it, moving X
	 * focus to a target could hand it the key typed here. */
	if (key->focused != NULL
	    && gowl_client_get_xwayland(GOWL_CLIENT(key->focused))
	    && !gowl_compositor_await_x11_key(self->compositor, key->keycode,
	                                      TRUE))
		bc_fail(self, "the focused X11 window did not confirm its own key");
	deliver_press(self, key->keycode, actions);
	g_array_unref(actions);
}

static void
bc_button_forwarded(
	GowlInputObserver             *observer,
	const GowlInputObserverButton *button
){
	GowlModuleBroadcast *self = GOWL_MODULE_BROADCAST(observer);
	g_autoptr(GArray) actions = NULL;
	GowlBroadcastWindow w;
	GArray *held;
	gpointer b = GUINT_TO_POINTER(button->button);
	guint i;
	guint n;

	if (!self->active || self->compositor == NULL)
		return;

	if (!button->pressed) {
		held = g_hash_table_lookup(self->held_buttons, b);
		for (i = 0; held != NULL && i < held->len; i++) {
			Held *h = &g_array_index(held, Held, i);
			GowlClient *c = client_by_id(self, h->client_id);

			if (c != NULL)
				gowl_compositor_deliver_button(self->compositor, c,
				                               h->code, FALSE, h->x, h->y);
		}
		g_hash_table_remove(self->held_buttons, b);
		return;
	}
	if (!self->on || button->synthetic || button->client == NULL
	    || button->width <= 0 || button->height <= 0)
		return;

	window_of(GOWL_CLIENT(button->client), &w);
	actions = g_array_new(FALSE, TRUE, sizeof(GowlBroadcastAction));
	if (!gowl_broadcast_engine_route_button(self->engine, &w, now_ms(),
	                                        actions)
	    || actions->len == 0)
		return;

	held = g_array_new(FALSE, TRUE, sizeof(Held));
	n = MIN(actions->len, self->max_targets);
	for (i = 0; i < n; i++) {
		GowlBroadcastAction *a = &g_array_index(actions,
		                                        GowlBroadcastAction, i);
		GowlClient *c = client_by_id(self, a->client_id);
		struct wlr_surface *surface;
		Held h;

		if (c == NULL)
			continue;
		surface = gowl_client_get_wlr_surface(c);
		if (surface == NULL)
			continue;
		memset(&h, 0, sizeof h);
		h.client_id = a->client_id;
		h.code = button->button;
		if (a->mouse == GOWL_BROADCAST_MOUSE_SCALED) {
			h.x = button->sx * (gdouble)surface->current.width
			      / (gdouble)button->width;
			h.y = button->sy * (gdouble)surface->current.height
			      / (gdouble)button->height;
		} else {
			h.x = button->sx;
			h.y = button->sy;
		}
		if (gowl_compositor_deliver_button(self->compositor, c,
		                                   button->button, TRUE, h.x, h.y)) {
			self->n_delivered++;
			g_array_append_val(held, h);
		} else {
			bc_fail(self, "a click was not delivered");
		}
	}
	g_hash_table_replace(self->held_buttons, b, held);
}

static void
bc_observer_init(GowlInputObserverInterface *iface)
{
	iface->filter_key       = bc_filter_key;
	iface->key_forwarded    = bc_key_forwarded;
	iface->button_forwarded = bc_button_forwarded;
}

/* ── IPC ─────────────────────────────────────────────────────────────── */

static const gchar *
delivery_text(GowlKeyDelivery d)
{
	switch (d) {
	case GOWL_KEY_DELIVERY_SEAT:     return "seat";
	case GOWL_KEY_DELIVERY_ACTIVATE: return "activate";
	case GOWL_KEY_DELIVERY_AUTO:
	default:                         return "auto";
	}
}

static gchar *
json_reply(JsonBuilder *b)
{
	g_autoptr(JsonGenerator) gen = NULL;
	g_autoptr(JsonNode) root = NULL;
	g_autofree gchar *text = NULL;

	root = json_builder_get_root(b);
	gen = json_generator_new();
	json_generator_set_root(gen, root);
	text = json_generator_to_data(gen, NULL);
	return g_strconcat("OK ", text, NULL);
}

static void
add_client_json(
	JsonBuilder *b,
	GowlClient  *c
){
	if (c == NULL) {
		json_builder_add_null_value(b);
		return;
	}
	json_builder_begin_object(b);
	json_builder_set_member_name(b, "id");
	json_builder_add_int_value(b, gowl_client_get_id(c));
	json_builder_set_member_name(b, "app_id");
	json_builder_add_string_value(b, gowl_client_get_app_id(c) != NULL
	                              ? gowl_client_get_app_id(c) : "");
	json_builder_set_member_name(b, "title");
	json_builder_add_string_value(b, gowl_client_get_title(c) != NULL
	                              ? gowl_client_get_title(c) : "");
	json_builder_set_member_name(b, "xwayland");
	json_builder_add_boolean_value(b, gowl_client_get_xwayland(c));
	json_builder_end_object(b);
}

static gchar *
cmd_status(GowlModuleBroadcast *self)
{
	g_autoptr(JsonBuilder) b = NULL;
	GowlClient *sel;
	const gchar *fteam = NULL;
	const gchar *fslot = NULL;
	guint t;
	guint r;

	bc_sync_config(self);
	b = json_builder_new();
	json_builder_begin_object(b);
	json_builder_set_member_name(b, "active");
	json_builder_add_boolean_value(b, self->active);
	json_builder_set_member_name(b, "on");
	json_builder_add_boolean_value(b, self->on);
	json_builder_set_member_name(b, "strict");
	json_builder_add_boolean_value(b,
		gowl_broadcast_engine_get_strict(self->engine));
	json_builder_set_member_name(b, "toggle_key");
	json_builder_add_string_value(b, self->toggle_text);
	json_builder_set_member_name(b, "panic_key");
	json_builder_add_string_value(b, self->panic_text);
	json_builder_set_member_name(b, "timeout_ms");
	json_builder_add_int_value(b, self->timeout_ms);
	json_builder_set_member_name(b, "max_targets");
	json_builder_add_int_value(b, self->max_targets);
	json_builder_set_member_name(b, "delivered");
	json_builder_add_int_value(b, (gint64)self->n_delivered);
	json_builder_set_member_name(b, "failed");
	json_builder_add_int_value(b, (gint64)self->n_failed);
	json_builder_set_member_name(b, "last_error");
	if (self->last_error != NULL)
		json_builder_add_string_value(b, self->last_error);
	else
		json_builder_add_null_value(b);
	json_builder_set_member_name(b, "held_keys");
	json_builder_add_int_value(b, g_hash_table_size(self->held_keys));
	json_builder_set_member_name(b, "queued");
	json_builder_add_int_value(b, g_queue_get_length(self->queue));

	sel = self->compositor != NULL
	      ? gowl_compositor_get_focused_client(self->compositor) : NULL;
	if (sel != NULL)
		gowl_broadcast_engine_slot_of(self->engine, gowl_client_get_id(sel),
		                              &fteam, &fslot);
	json_builder_set_member_name(b, "focused");
	json_builder_begin_object(b);
	json_builder_set_member_name(b, "client");
	add_client_json(b, sel);
	json_builder_set_member_name(b, "team");
	if (fteam != NULL)
		json_builder_add_string_value(b, fteam);
	else
		json_builder_add_null_value(b);
	json_builder_set_member_name(b, "slot");
	if (fslot != NULL)
		json_builder_add_string_value(b, fslot);
	else
		json_builder_add_null_value(b);
	json_builder_end_object(b);

	json_builder_set_member_name(b, "teams");
	json_builder_begin_array(b);
	for (t = 0; t < gowl_broadcast_engine_get_n_teams(self->engine); t++) {
		GowlBroadcastTeam *team = gowl_broadcast_engine_get_team(
			self->engine, t);
		const gchar *tname = gowl_broadcast_team_get_name(team);
		g_auto(GStrv) groups = NULL;
		guint s;

		json_builder_begin_object(b);
		json_builder_set_member_name(b, "name");
		json_builder_add_string_value(b, tname);
		json_builder_set_member_name(b, "source");
		json_builder_add_string_value(b,
			gowl_broadcast_engine_team_is_runtime(self->engine, team)
			? "runtime" : "config");
		json_builder_set_member_name(b, "slots");
		json_builder_begin_array(b);
		for (s = 0; s < gowl_broadcast_team_get_n_slots(team); s++) {
			const gchar *v;

			json_builder_begin_object(b);
			json_builder_set_member_name(b, "name");
			json_builder_add_string_value(b,
				gowl_broadcast_team_get_slot_name(team, s));
			json_builder_set_member_name(b, "match");
			v = gowl_broadcast_team_get_slot_match(team, s);
			if (v != NULL)
				json_builder_add_string_value(b, v);
			else
				json_builder_add_null_value(b);
			json_builder_set_member_name(b, "focus_key");
			v = gowl_broadcast_team_get_slot_focus_key(team, s);
			if (v != NULL)
				json_builder_add_string_value(b, v);
			else
				json_builder_add_null_value(b);
			json_builder_set_member_name(b, "delivery");
			json_builder_add_string_value(b, delivery_text(
				gowl_broadcast_team_get_slot_delivery(team, s)));
			json_builder_set_member_name(b, "client");
			add_client_json(b, client_by_id(self,
				gowl_broadcast_engine_client_for(self->engine, tname, s)));
			json_builder_end_object(b);
		}
		json_builder_end_array(b);
		json_builder_set_member_name(b, "groups");
		json_builder_begin_object(b);
		groups = gowl_broadcast_team_get_group_names(team);
		for (s = 0; groups[s] != NULL; s++) {
			const gchar * const *members;
			guint m;

			json_builder_set_member_name(b, groups[s]);
			json_builder_begin_array(b);
			members = gowl_broadcast_team_get_group(team, groups[s]);
			for (m = 0; members != NULL && members[m] != NULL; m++)
				json_builder_add_string_value(b, members[m]);
			json_builder_end_array(b);
		}
		json_builder_end_object(b);
		json_builder_end_object(b);
	}
	json_builder_end_array(b);

	json_builder_set_member_name(b, "rules");
	json_builder_begin_array(b);
	for (r = 0; r < gowl_broadcast_engine_get_n_rules(self->engine); r++) {
		GowlBroadcastRule *rule = gowl_broadcast_engine_get_rule(
			self->engine, r);
		g_autofree gchar *yaml = gowl_broadcast_rule_to_yaml(rule);
		const gchar *problem;

		json_builder_begin_object(b);
		json_builder_set_member_name(b, "name");
		json_builder_add_string_value(b, gowl_broadcast_rule_get_name(rule));
		json_builder_set_member_name(b, "team");
		json_builder_add_string_value(b,
			gowl_broadcast_rule_get_team(rule) != NULL
			? gowl_broadcast_rule_get_team(rule) : "");
		json_builder_set_member_name(b, "enabled");
		json_builder_add_boolean_value(b,
			gowl_broadcast_rule_get_enabled(rule));
		json_builder_set_member_name(b, "source");
		json_builder_add_string_value(b,
			gowl_broadcast_engine_rule_is_runtime(self->engine, rule)
			? "runtime" : "config");
		json_builder_set_member_name(b, "problem");
		problem = gowl_broadcast_engine_rule_problem(self->engine, rule);
		if (problem != NULL)
			json_builder_add_string_value(b, problem);
		else
			json_builder_add_null_value(b);
		json_builder_set_member_name(b, "yaml");
		json_builder_add_string_value(b, yaml);
		json_builder_end_object(b);
	}
	json_builder_end_array(b);
	json_builder_end_object(b);
	return json_reply(b);
}

static gchar *
cmd_list(GowlModuleBroadcast *self)
{
	g_autoptr(JsonBuilder) b = NULL;
	guint i;

	bc_sync_config(self);
	b = json_builder_new();
	json_builder_begin_object(b);
	json_builder_set_member_name(b, "teams");
	json_builder_begin_array(b);
	for (i = 0; i < gowl_broadcast_engine_get_n_teams(self->engine); i++) {
		g_autofree gchar *yaml = gowl_broadcast_team_to_yaml(
			gowl_broadcast_engine_get_team(self->engine, i));

		json_builder_add_string_value(b, yaml);
	}
	json_builder_end_array(b);
	json_builder_set_member_name(b, "rules");
	json_builder_begin_array(b);
	for (i = 0; i < gowl_broadcast_engine_get_n_rules(self->engine); i++) {
		g_autofree gchar *yaml = gowl_broadcast_rule_to_yaml(
			gowl_broadcast_engine_get_rule(self->engine, i));

		json_builder_add_string_value(b, yaml);
	}
	json_builder_end_array(b);
	json_builder_end_object(b);
	return json_reply(b);
}

static gchar *
cmd_add_team(
	GowlModuleBroadcast *self,
	const gchar         *args
){
	g_autoptr(GowlBroadcastTeam) team = NULL;
	g_autoptr(GError) error = NULL;
	gboolean replaced;

	if (args == NULL || *args == '\0')
		return g_strdup("ERROR broadcast-add-team needs a team, e.g. "
		                "{name: eq, slots: [{name: main, match: "
		                "'app-id = eqgame.exe'}]}");
	team = gowl_broadcast_team_new_from_yaml(args, &error);
	if (team == NULL)
		return g_strdup_printf("ERROR %s", error->message);
	bc_sync_config(self);
	release_all(self);
	replaced = gowl_broadcast_engine_add_team(self->engine, team);
	bc_assign(self, NULL);
	return g_strdup_printf("OK %s %s", replaced ? "replaced" : "added",
	                       gowl_broadcast_team_get_name(team));
}

static gchar *
cmd_add_rule(
	GowlModuleBroadcast *self,
	const gchar         *args
){
	g_autoptr(GowlBroadcastRule) rule = NULL;
	g_autoptr(GError) error = NULL;
	const gchar *problem;
	gboolean replaced;

	if (args == NULL || *args == '\0')
		return g_strdup("ERROR broadcast-add-rule needs a rule, e.g. "
		                "{name: follow, team: eq, targets: all-other}");
	rule = gowl_broadcast_rule_new_from_yaml(args, &error);
	if (rule == NULL)
		return g_strdup_printf("ERROR %s", error->message);
	bc_sync_config(self);
	replaced = gowl_broadcast_engine_add_rule(self->engine, rule);
	problem = gowl_broadcast_engine_rule_problem(self->engine, rule);
	if (problem != NULL)
		return g_strdup_printf("OK %s %s (cannot run: %s)",
		                       replaced ? "replaced" : "added",
		                       gowl_broadcast_rule_get_name(rule), problem);
	return g_strdup_printf("OK %s %s", replaced ? "replaced" : "added",
	                       gowl_broadcast_rule_get_name(rule));
}

/* "TEAM SLOT [ID]" or "SLOT" (the focused window's team) */
static gchar **
split_args(const gchar *args)
{
	g_autofree gchar *copy = NULL;

	if (args == NULL)
		return g_new0(gchar *, 1);
	copy = g_strstrip(g_strdup(args));
	if (*copy == '\0')
		return g_new0(gchar *, 1);
	return g_regex_split_simple("\\s+", copy, 0, 0);
}

static gchar *
cmd_assign(
	GowlModuleBroadcast *self,
	const gchar         *args,
	gboolean             assign
){
	g_auto(GStrv) argv = split_args(args);
	g_autoptr(GError) error = NULL;
	guint64 id = 0;

	if (g_strv_length(argv) < 2)
		return g_strdup_printf("ERROR broadcast-%s TEAM SLOT%s",
		                       assign ? "assign" : "unassign",
		                       assign ? " [CLIENT-ID]" : "");
	bc_sync_config(self);
	if (assign) {
		if (argv[2] != NULL) {
			if (!g_ascii_string_to_unsigned(argv[2], 10, 1, G_MAXUINT,
			                                &id, NULL)
			    || client_by_id(self, (guint)id) == NULL)
				return g_strdup_printf("ERROR no window with id %s",
				                       argv[2]);
		} else {
			GowlClient *sel = self->compositor != NULL
				? gowl_compositor_get_focused_client(self->compositor)
				: NULL;

			if (sel == NULL)
				return g_strdup("ERROR no window has focus; give an id");
			id = gowl_client_get_id(sel);
		}
	}
	if (!gowl_broadcast_engine_set_manual(self->engine, argv[0], argv[1],
	                                      (guint)id, &error))
		return g_strdup_printf("ERROR %s", error->message);
	bc_assign(self, NULL);
	return assign ? g_strdup_printf("OK %s %s = %u", argv[0], argv[1],
	                                (guint)id)
	              : g_strdup_printf("OK %s %s by match", argv[0], argv[1]);
}

static gchar *
cmd_focus_slot(
	GowlModuleBroadcast *self,
	const gchar         *args
){
	g_auto(GStrv) argv = split_args(args);
	GowlBroadcastTeam *team;
	const gchar *tname;
	const gchar *sname;
	GowlClient *c;
	gint s;

	bc_sync_config(self);
	if (g_strv_length(argv) >= 2) {
		tname = argv[0];
		sname = argv[1];
	} else if (g_strv_length(argv) == 1) {
		GowlClient *sel = self->compositor != NULL
			? gowl_compositor_get_focused_client(self->compositor) : NULL;

		tname = NULL;
		if (sel != NULL)
			gowl_broadcast_engine_slot_of(self->engine,
			                              gowl_client_get_id(sel),
			                              &tname, NULL);
		if (tname == NULL && gowl_broadcast_engine_get_n_teams(
			    self->engine) > 0)
			tname = gowl_broadcast_team_get_name(
				gowl_broadcast_engine_get_team(self->engine, 0));
		sname = argv[0];
	} else {
		return g_strdup("ERROR broadcast-focus-slot [TEAM] SLOT");
	}
	team = gowl_broadcast_engine_find_team(self->engine, tname);
	if (team == NULL)
		return g_strdup_printf("ERROR no team named %s",
		                       tname != NULL ? tname : "(none)");
	s = gowl_broadcast_team_find_slot(team, sname);
	if (s < 0)
		return g_strdup_printf("ERROR team %s has no slot %s", tname, sname);
	c = client_by_id(self, gowl_broadcast_engine_client_for(self->engine,
	                                                        tname, (guint)s));
	if (c == NULL)
		return g_strdup_printf("ERROR slot %s of %s is empty", sname, tname);
	gowl_compositor_show_client(self->compositor, c);
	return g_strdup_printf("OK %u", gowl_client_get_id(c));
}

static gchar *
cmd_strict(
	GowlModuleBroadcast *self,
	const gchar         *args
){
	gboolean want;
	g_autofree gchar *a = g_strstrip(g_strdup(args != NULL ? args : ""));

	if (*a == '\0')
		return g_strdup_printf("OK %s",
			gowl_broadcast_engine_get_strict(self->engine) ? "on" : "off");
	if (g_ascii_strcasecmp(a, "on") == 0)
		want = TRUE;
	else if (g_ascii_strcasecmp(a, "off") == 0)
		want = FALSE;
	else if (g_ascii_strcasecmp(a, "toggle") == 0)
		want = !gowl_broadcast_engine_get_strict(self->engine);
	else
		return g_strdup("ERROR broadcast-strict on|off|toggle");
	release_all(self);
	gowl_broadcast_engine_set_strict(self->engine, want);
	gowl_compositor_emit_module_event(self->compositor, "broadcast",
	                                  "strict %s", want ? "on" : "off");
	return g_strdup_printf("OK %s (%u rule(s) cannot run)",
		want ? "on" : "off",
		gowl_broadcast_engine_get_n_problems(self->engine));
}

static gchar *
bc_handle_command(
	GowlIpcHandler *handler,
	const gchar    *command,
	const gchar    *args
){
	GowlModuleBroadcast *self;
	const gchar *verb;
	g_autofree gchar *arg = NULL;

	if (command == NULL || !g_str_has_prefix(command, BC_PREFIX))
		return NULL;
	self = GOWL_MODULE_BROADCAST(handler);
	verb = command + strlen(BC_PREFIX);
	arg = g_strstrip(g_strdup(args != NULL ? args : ""));

	if (g_strcmp0(verb, "status") == 0)
		return cmd_status(self);
	if (g_strcmp0(verb, "list") == 0)
		return cmd_list(self);
	if (!self->active)
		return g_strdup("ERROR the broadcast module is not active");
	if (g_strcmp0(verb, "on") == 0) {
		if (!self->on)
			set_on(self, TRUE, "ipc");
		return g_strdup("OK on");
	}
	if (g_strcmp0(verb, "off") == 0) {
		if (self->on)
			set_on(self, FALSE, "ipc");
		return g_strdup("OK off");
	}
	if (g_strcmp0(verb, "toggle") == 0) {
		set_on(self, !self->on, "ipc");
		return g_strdup_printf("OK %s", self->on ? "on" : "off");
	}
	if (g_strcmp0(verb, "panic") == 0) {
		do_panic(self);
		return g_strdup("OK off");
	}
	if (g_strcmp0(verb, "add-team") == 0)
		return cmd_add_team(self, args);
	if (g_strcmp0(verb, "add-rule") == 0)
		return cmd_add_rule(self, args);
	if (g_strcmp0(verb, "remove-team") == 0) {
		bc_sync_config(self);
		release_all(self);
		if (!gowl_broadcast_engine_remove_team(self->engine, arg))
			return g_strdup_printf("ERROR no team named %s", arg);
		bc_assign(self, NULL);
		return g_strdup_printf("OK removed %s", arg);
	}
	if (g_strcmp0(verb, "remove-rule") == 0) {
		bc_sync_config(self);
		if (!gowl_broadcast_engine_remove_rule(self->engine, arg))
			return g_strdup_printf("ERROR no rule named %s", arg);
		return g_strdup_printf("OK removed %s", arg);
	}
	if (g_strcmp0(verb, "enable-rule") == 0
	    || g_strcmp0(verb, "disable-rule") == 0) {
		gboolean en = verb[0] == 'e';

		bc_sync_config(self);
		if (!gowl_broadcast_engine_set_rule_enabled(self->engine, arg, en))
			return g_strdup_printf("ERROR no rule named %s", arg);
		return g_strdup_printf("OK %s %s", en ? "enabled" : "disabled",
		                       arg);
	}
	if (g_strcmp0(verb, "clear") == 0) {
		release_all(self);
		gowl_broadcast_engine_clear_runtime(self->engine);
		bc_assign(self, NULL);
		return g_strdup("OK cleared");
	}
	if (g_strcmp0(verb, "reload") == 0) {
		release_all(self);
		if (self->config != NULL)
			gowl_broadcast_engine_set_config(self->engine,
				gowl_config_get_broadcast_teams(self->config),
				gowl_config_get_broadcast_rules(self->config));
		bc_sync_config(self);
		bc_assign(self, NULL);
		return g_strdup_printf("OK %u team(s), %u rule(s)",
			gowl_broadcast_engine_get_n_teams(self->engine),
			gowl_broadcast_engine_get_n_rules(self->engine));
	}
	if (g_strcmp0(verb, "assign") == 0)
		return cmd_assign(self, args, TRUE);
	if (g_strcmp0(verb, "unassign") == 0)
		return cmd_assign(self, args, FALSE);
	if (g_strcmp0(verb, "focus-slot") == 0)
		return cmd_focus_slot(self, args);
	if (g_strcmp0(verb, "step-reset") == 0) {
		gowl_broadcast_engine_reset_steps(self->engine,
			*arg != '\0' ? arg : NULL);
		return g_strdup("OK reset");
	}
	if (g_strcmp0(verb, "strict") == 0)
		return cmd_strict(self, args);
	return g_strdup_printf("ERROR unknown command %s; the broadcast module "
	                       "knows " BC_PREFIX "on, -off, -toggle, -panic, "
	                       "-status, -list, -add-team, -add-rule, "
	                       "-remove-team, -remove-rule, -enable-rule, "
	                       "-disable-rule, -clear, -reload, -assign, "
	                       "-unassign, -focus-slot, -step-reset and -strict",
	                       command);
}

static void
bc_ipc_init(GowlIpcHandlerInterface *iface)
{
	iface->handle_command = bc_handle_command;
}

/* ── Window tracking ─────────────────────────────────────────────────── */

/* A held key's window is gone: nothing to release it in. */
static void
forget_client(
	GowlModuleBroadcast *self,
	guint                id
){
	GHashTableIter iter;
	gpointer value;
	GList *q;

	g_hash_table_iter_init(&iter, self->held_keys);
	while (g_hash_table_iter_next(&iter, NULL, &value)) {
		GArray *held = value;
		guint i;

		for (i = 0; i < held->len; )
			if (g_array_index(held, Held, i).client_id == id)
				g_array_remove_index(held, i);
			else
				i++;
	}
	g_hash_table_iter_init(&iter, self->held_buttons);
	while (g_hash_table_iter_next(&iter, NULL, &value)) {
		GArray *held = value;
		guint i;

		for (i = 0; i < held->len; )
			if (g_array_index(held, Held, i).client_id == id)
				g_array_remove_index(held, i);
			else
				i++;
	}
	for (q = self->queue->head; q != NULL; q = q->next)
		if (((Queued *)q->data)->client_id == id)
			((Queued *)q->data)->client_id = 0;
}

static void
on_client_added(
	GowlCompositor *compositor,
	GowlClient     *client,
	gpointer        data
){
	(void)compositor;
	(void)client;
	bc_assign(GOWL_MODULE_BROADCAST(data), NULL);
}

static void
on_client_removed(
	GowlCompositor *compositor,
	GowlClient     *client,
	gpointer        data
){
	GowlModuleBroadcast *self = GOWL_MODULE_BROADCAST(data);

	(void)compositor;
	if (client != NULL)
		forget_client(self, gowl_client_get_id(client));
	bc_assign(self, client);
}

static void
on_title_changed(
	GowlCompositor *compositor,
	GowlClient     *client,
	gpointer        data
){
	(void)compositor;
	(void)client;
	bc_assign(GOWL_MODULE_BROADCAST(data), NULL);
}

/* A lock is no time to leave keys held in background windows. */
static void
on_lock_changed(
	GowlCompositor *compositor,
	gboolean        locked,
	gpointer        data
){
	GowlModuleBroadcast *self = GOWL_MODULE_BROADCAST(data);

	(void)compositor;
	if (locked && self->on) {
		release_all(self);
		self->on = FALSE;
		gowl_compositor_emit_module_event(self->compositor, "broadcast",
		                                  "off lock");
	}
}

/* ── GowlModule ──────────────────────────────────────────────────────── */

static gboolean
parse_bool_setting(
	const gchar *value,
	gboolean    *out
){
	if (g_ascii_strcasecmp(value, "true") == 0
	    || g_ascii_strcasecmp(value, "yes") == 0
	    || g_ascii_strcasecmp(value, "on") == 0
	    || g_strcmp0(value, "1") == 0)
		*out = TRUE;
	else if (g_ascii_strcasecmp(value, "false") == 0
	         || g_ascii_strcasecmp(value, "no") == 0
	         || g_ascii_strcasecmp(value, "off") == 0
	         || g_strcmp0(value, "0") == 0)
		*out = FALSE;
	else
		return FALSE;
	return TRUE;
}

/* A keybind setting; "none" turns the key off. */
static void
set_bind(
	const gchar  *what,
	const gchar  *value,
	gchar       **text,
	guint        *mods,
	guint        *sym
){
	guint m = 0;
	guint s = 0;

	if (g_ascii_strcasecmp(value, "none") == 0 || *value == '\0') {
		g_free(*text);
		*text = g_strdup("none");
		*mods = 0;
		*sym = 0;
		return;
	}
	if (!gowl_keybind_parse(value, &m, &s) || s == 0) {
		g_warning("broadcast: %s '%s' is no keybind; keeping %s", what,
		          value, *text);
		return;
	}
	g_free(*text);
	*text = g_strdup(value);
	*mods = m;
	*sym = s;
}

/*
 * bc_configure:
 * @mod: the module
 * @config: a #GHashTable of string settings from `modules: broadcast:'
 *
 * toggle-key, panic-key (keybinds, or "none"), start-on, strict, toast,
 * log (booleans), max-targets (1..64), timeout-ms (5..2000).  A bad
 * value is refused with a warning and the old one kept.  Teams and
 * rules live in the top-level `broadcast:' section, not here.
 */
static void
bc_configure(
	GowlModule *mod,
	gpointer    config
){
	GowlModuleBroadcast *self = GOWL_MODULE_BROADCAST(mod);
	GHashTable *settings = (GHashTable *)config;
	const gchar *value;
	gboolean b;
	guint64 n;

	if (settings == NULL)
		return;

	if ((value = g_hash_table_lookup(settings, "toggle-key")) != NULL)
		set_bind("toggle-key", value, &self->toggle_text,
		         &self->toggle_mods, &self->toggle_sym);
	if ((value = g_hash_table_lookup(settings, "panic-key")) != NULL)
		set_bind("panic-key", value, &self->panic_text,
		         &self->panic_mods, &self->panic_sym);
	if ((value = g_hash_table_lookup(settings, "start-on")) != NULL) {
		if (parse_bool_setting(value, &b))
			self->start_on = b;
		else
			g_warning("broadcast: start-on '%s' is no yes/no", value);
	}
	if ((value = g_hash_table_lookup(settings, "strict")) != NULL) {
		if (parse_bool_setting(value, &b))
			gowl_broadcast_engine_set_strict(self->engine, b);
		else
			g_warning("broadcast: strict '%s' is no yes/no", value);
	}
	if ((value = g_hash_table_lookup(settings, "toast")) != NULL) {
		if (parse_bool_setting(value, &b))
			self->toast = b;
		else
			g_warning("broadcast: toast '%s' is no yes/no", value);
	}
	if ((value = g_hash_table_lookup(settings, "log")) != NULL) {
		if (parse_bool_setting(value, &b))
			self->log = b;
		else
			g_warning("broadcast: log '%s' is no yes/no", value);
	}
	if ((value = g_hash_table_lookup(settings, "max-targets")) != NULL) {
		if (g_ascii_string_to_unsigned(value, 10, 1, BC_HARD_MAX_TARGETS,
		                               &n, NULL))
			self->max_targets = (guint)n;
		else
			g_warning("broadcast: max-targets '%s' -- 1..%d", value,
			          BC_HARD_MAX_TARGETS);
	}
	if ((value = g_hash_table_lookup(settings, "timeout-ms")) != NULL) {
		if (g_ascii_string_to_unsigned(value, 10, 5, 2000, &n, NULL)) {
			self->timeout_ms = (guint)n;
			if (self->compositor != NULL)
				gowl_compositor_set_delivery_timeout(self->compositor,
				                                     self->timeout_ms);
		} else {
			g_warning("broadcast: timeout-ms '%s' -- 5..2000", value);
		}
	}
}

static gboolean
bc_activate(GowlModule *mod)
{
	GowlModuleBroadcast *self = GOWL_MODULE_BROADCAST(mod);

	self->active = TRUE;
	/* Enabled after startup (cmacs's gowl-enable-module): the
	 * compositor is already known.  At first start the startup handler
	 * does this instead. */
	bc_sync_config(self);
	bc_assign(self, NULL);
	return TRUE;
}

static void
bc_deactivate(GowlModule *mod)
{
	GowlModuleBroadcast *self = GOWL_MODULE_BROADCAST(mod);

	release_all(self);
	queue_clear(self);
	if (self->on && self->compositor != NULL)
		gowl_compositor_emit_module_event(self->compositor, "broadcast",
		                                  "off deactivated");
	self->on = FALSE;
	self->active = FALSE;
}

static const gchar *
bc_get_name(GowlModule *mod)
{
	(void)mod;
	return "broadcast";
}

static const gchar *
bc_get_description(GowlModule *mod)
{
	(void)mod;
	return "Input broadcasting for multiboxing: repeat keys and clicks "
	       "into the other windows of a team";
}

static const gchar *
bc_get_version(GowlModule *mod)
{
	(void)mod;
	return "0.1.0";
}

/* ── Startup / shutdown ──────────────────────────────────────────────── */

/* The queue timer belongs to the display's event loop. */
static void
bc_display_destroyed(
	struct wl_listener *listener,
	void               *data
){
	GowlModuleBroadcast *self;

	(void)data;
	self = wl_container_of(listener, self, display_destroy);
	if (self->queue_timer != NULL) {
		wl_event_source_remove(self->queue_timer);
		self->queue_timer = NULL;
	}
	g_queue_clear_full(self->queue, g_free);
	wl_list_remove(&self->display_destroy.link);
	wl_list_init(&self->display_destroy.link);
}

static void
bc_on_startup(
	GowlStartupHandler *handler,
	gpointer            compositor
){
	GowlModuleBroadcast *self = GOWL_MODULE_BROADCAST(handler);
	struct wl_display *display;

	if (self->compositor == NULL) {
		self->compositor = GOWL_COMPOSITOR(compositor);
		g_object_add_weak_pointer(G_OBJECT(compositor),
		                          (gpointer *)&self->compositor);
		g_signal_connect_object(compositor, "client-added",
		                        G_CALLBACK(on_client_added), self, 0);
		g_signal_connect_object(compositor, "client-removed",
		                        G_CALLBACK(on_client_removed), self, 0);
		g_signal_connect_object(compositor, "client-title-changed",
		                        G_CALLBACK(on_title_changed), self, 0);
		g_signal_connect_object(compositor, "lock-changed",
		                        G_CALLBACK(on_lock_changed), self, 0);
	}
	display = gowl_compositor_get_wl_display(self->compositor);
	if (display != NULL && wl_list_empty(&self->display_destroy.link))
		wl_display_add_destroy_listener(display, &self->display_destroy);
	gowl_compositor_set_delivery_timeout(self->compositor, self->timeout_ms);

	bc_sync_config(self);
	bc_assign(self, NULL);
	bc_log(self, "broadcast: started with %u team(s), %u rule(s); %s "
	       "toggles",
	       gowl_broadcast_engine_get_n_teams(self->engine),
	       gowl_broadcast_engine_get_n_rules(self->engine),
	       self->toggle_text);
	if (self->start_on && self->active && !self->on)
		set_on(self, TRUE, "start-on");
}

static void
bc_startup_init(GowlStartupHandlerInterface *iface)
{
	iface->on_startup = bc_on_startup;
}

/* Shutdown: let go of every key while the compositor can still send. */
static void
bc_on_shutdown(
	GowlShutdownHandler *handler,
	gpointer             compositor
){
	GowlModuleBroadcast *self = GOWL_MODULE_BROADCAST(handler);

	(void)compositor;
	release_all(self);
	queue_clear(self);
	self->on = FALSE;
}

static void
bc_shutdown_init(GowlShutdownHandlerInterface *iface)
{
	iface->on_shutdown = bc_on_shutdown;
}

/* ── GObject lifecycle ───────────────────────────────────────────────── */

static void
gowl_module_broadcast_finalize(GObject *object)
{
	GowlModuleBroadcast *self = GOWL_MODULE_BROADCAST(object);

	wl_list_remove(&self->display_destroy.link);
	if (self->queue_timer != NULL)
		wl_event_source_remove(self->queue_timer);
	g_queue_free_full(self->queue, g_free);
	if (self->config != NULL && self->reloaded_handler != 0)
		g_signal_handler_disconnect(self->config, self->reloaded_handler);
	g_clear_object(&self->config);
	if (self->compositor != NULL)
		g_object_remove_weak_pointer(G_OBJECT(self->compositor),
		                             (gpointer *)&self->compositor);
	gowl_broadcast_engine_free(self->engine);
	g_hash_table_unref(self->held_keys);
	g_hash_table_unref(self->held_buttons);
	g_hash_table_unref(self->pending);
	g_hash_table_unref(self->swallowed);
	g_hash_table_unref(self->release_due);
	g_free(self->toggle_text);
	g_free(self->panic_text);
	g_free(self->last_error);

	G_OBJECT_CLASS(gowl_module_broadcast_parent_class)->finalize(object);
}

static void
gowl_module_broadcast_class_init(GowlModuleBroadcastClass *klass)
{
	GObjectClass    *object_class = G_OBJECT_CLASS(klass);
	GowlModuleClass *module_class = GOWL_MODULE_CLASS(klass);

	object_class->finalize = gowl_module_broadcast_finalize;

	module_class->activate        = bc_activate;
	module_class->deactivate      = bc_deactivate;
	module_class->get_name        = bc_get_name;
	module_class->get_description = bc_get_description;
	module_class->get_version     = bc_get_version;
	module_class->configure       = bc_configure;
}

static void
gowl_module_broadcast_init(GowlModuleBroadcast *self)
{
	wl_list_init(&self->display_destroy.link);
	self->display_destroy.notify = bc_display_destroyed;
	self->engine = gowl_broadcast_engine_new();
	self->toast = TRUE;
	self->max_targets = BC_DEFAULT_MAX_TARGETS;
	self->timeout_ms = BC_DEFAULT_TIMEOUT_MS;
	self->toggle_text = g_strdup("none");
	self->panic_text = g_strdup("none");
	set_bind("toggle-key", BC_DEFAULT_TOGGLE, &self->toggle_text,
	         &self->toggle_mods, &self->toggle_sym);
	set_bind("panic-key", BC_DEFAULT_PANIC, &self->panic_text,
	         &self->panic_mods, &self->panic_sym);
	self->held_keys = g_hash_table_new_full(g_direct_hash, g_direct_equal,
	                                        NULL,
	                                        (GDestroyNotify)g_array_unref);
	self->held_buttons = g_hash_table_new_full(g_direct_hash,
	                                           g_direct_equal, NULL,
	                                           (GDestroyNotify)g_array_unref);
	self->pending = g_hash_table_new_full(g_direct_hash, g_direct_equal,
	                                      NULL,
	                                      (GDestroyNotify)g_array_unref);
	self->swallowed = g_hash_table_new(g_direct_hash, g_direct_equal);
	self->release_due = g_hash_table_new(g_direct_hash, g_direct_equal);
	self->queue = g_queue_new();
}

/* ── Shared-object entry point ───────────────────────────────────────── */

G_MODULE_EXPORT GType
gowl_module_register(void)
{
	return GOWL_TYPE_MODULE_BROADCAST;
}
