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
 * gowl-module-hotkey.c - AutoHotkey for gowl (opt-in).
 *
 * Hotkeys with AutoHotkey's semantics -- `#!t', `~LButton', `CapsLock &
 * j', `^a UP', double taps, long presses -- remaps, modes, contexts
 * (#HotIf), hotstrings (handed to gabbrev), timers and event hooks,
 * each running a list of actions: gowl actions and commands, wire
 * protocol methods, macros, shell, crispy C, functions, Send, window and
 * mouse control, and under cmacs Elisp.  docs/hotkey.org is the manual.
 *
 * Loaded only when asked for: `modules: hotkey: {enabled: true}' in
 * gowl's YAML, (gowl-enable-module "hotkey") in cmacs.
 *
 * How an input travels:
 *
 *   filter_key (early: before every bind and shortcuts-inhibit, so a
 *   hotkey works in a fullscreen game the way AutoHotkey's hook does):
 *       the suspend key, then an input hook, then the engine
 *       (gowl-hotkey-engine.c), which decides to swallow the key or not
 *       and what follows -- hotkeys to fire, keys to put back.  Keys put
 *       back for a swallowed key are injected right there (the key that
 *       caused them reaches nothing else, so nothing re-enters a half
 *       done decision); hotkeys fire a loop turn later.
 *   handle_button / handle_axis (GowlMouseHandler): the same for
 *       buttons and wheel notches.
 *
 * Our own injected input re-enters the hooks; `injecting' marks it and
 * the engine fires only hotkeys below its send level on it.
 *
 * IPC (replies "OK ...", "OK <json>" or "ERROR ..."); the full list is
 * in docs/hotkey.org and `hotkey-help':
 *
 *   hotkey-status | -list [json] | -add YAML | -remove ID | -enable ID
 *   -disable ID | -toggle ID | -suspend [on|off|toggle] | -mode [NAME]
 *   -send KEYS | -send-text TEXT | -fire ID | -call NAME [ARGS] | -reload
 *   -history | -keystate KEY | -capslock STATE | -numlock STATE
 *   -context-set NAME 0|1 | -context-clear NAME | -contexts
 *   -func-define NAME FORM | -func-undefine NAME | -functions
 *   -input-start [NAME] [OPTIONS] | -input-stop | -problems | -timers
 *   -timer-enable ID | -timer-disable ID | -cancel | -help
 */

#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "gowl-hotkey"

#include <glib-object.h>
#include <gmodule.h>
#include <json-glib/json-glib.h>
#include <linux/input-event-codes.h>
#include <string.h>
#include <wayland-server-core.h>
#include <xkbcommon/xkbcommon.h>

#include "gowl-hotkey-module.h"
#include "gowl-hotkey-hotstring.h"

#include "interfaces/gowl-input-observer.h"
#include "interfaces/gowl-mouse-handler.h"
#include "interfaces/gowl-ipc-handler.h"
#include "interfaces/gowl-startup-handler.h"
#include "interfaces/gowl-shutdown-handler.h"
#include "core/gowl-core-private.h"
#include "core/gowl-client.h"
#include "core/gowl-monitor.h"
#include "config/gowl-keybind.h"
#include "gabbrev/gowl-gabbrev.h"
#include "hotkey/gowl-hotkey-private.h"

static void hk_observer_init (GowlInputObserverInterface   *iface);
static void hk_mouse_init    (GowlMouseHandlerInterface    *iface);
static void hk_ipc_init      (GowlIpcHandlerInterface      *iface);
static void hk_startup_init  (GowlStartupHandlerInterface  *iface);
static void hk_shutdown_init (GowlShutdownHandlerInterface *iface);

G_DEFINE_TYPE_WITH_CODE(GowlModuleHotkey, gowl_module_hotkey, GOWL_TYPE_MODULE,
	G_IMPLEMENT_INTERFACE(GOWL_TYPE_INPUT_OBSERVER, hk_observer_init)
	G_IMPLEMENT_INTERFACE(GOWL_TYPE_MOUSE_HANDLER, hk_mouse_init)
	G_IMPLEMENT_INTERFACE(GOWL_TYPE_IPC_HANDLER, hk_ipc_init)
	G_IMPLEMENT_INTERFACE(GOWL_TYPE_STARTUP_HANDLER, hk_startup_init)
	G_IMPLEMENT_INTERFACE(GOWL_TYPE_SHUTDOWN_HANDLER, hk_shutdown_init))

/* ── Small helpers ───────────────────────────────────────────────────── */

static gint64
now_ms(void)
{
	return g_get_monotonic_time() / 1000;
}

/* A line for the journal: routine at info, everything when `log'. */
void
hk_log(
	GowlModuleHotkey *self,
	const gchar      *format,
	...
){
	g_autofree gchar *msg = NULL;
	va_list ap;

	va_start(ap, format);
	msg = g_strdup_vprintf(format, ap);
	va_end(ap);
	if (self->log)
		g_message("hotkey: %s", msg);
	else
		g_info("hotkey: %s", msg);
}

/* A failure: counted, kept for hotkey-status, announced as an event. */
void
hk_fail(
	GowlModuleHotkey *self,
	const gchar      *format,
	...
){
	g_autofree gchar *msg = NULL;
	va_list ap;

	va_start(ap, format);
	msg = g_strdup_vprintf(format, ap);
	va_end(ap);
	self->n_failed++;
	g_free(self->last_error);
	self->last_error = g_strdup(msg);
	g_message("hotkey: %s", msg);
	if (self->compositor != NULL)
		gowl_compositor_emit_module_event(self->compositor, "hotkey",
		                                  "fault %s", msg);
	if (self->dbus_service != NULL)
		hk_dbus_emit(self->dbus_service, "Fault", msg, "");
}

/* A toast (the layout indicator draws it; the bar too when loaded) and
 * a module event, which cmacs and IPC subscribers hear. */
void
hk_announce(
	GowlModuleHotkey *self,
	const gchar      *toast,
	const gchar      *format,
	...
){
	if (self->compositor == NULL)
		return;
	if (self->toast && toast != NULL && *toast != '\0') {
		g_autofree gchar *flat = NULL;
		g_autofree gchar *line = NULL;

		flat = g_strdelimit(g_strdup(toast), "|\n", ' ');
		g_signal_emit_by_name(self->compositor, "toast-requested",
			gowl_compositor_get_selected_monitor(self->compositor), flat);
		line = g_strdup_printf("bar-notify %s|", flat);
		g_free(gowl_compositor_run_command(self->compositor, line));
	}
	if (format != NULL) {
		g_autofree gchar *msg = NULL;
		va_list ap;

		va_start(ap, format);
		msg = g_strdup_vprintf(format, ap);
		va_end(ap);
		gowl_compositor_emit_module_event(self->compositor, "hotkey", "%s",
		                                  msg);
	}
}

GowlClient *
hk_client_by_id(
	GowlModuleHotkey *self,
	guint             id
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

/* The program behind a window: /proc/PID/exe */
static gchar *
exec_of(GowlClient *c)
{
	g_autofree gchar *link = NULL;
	gint pid = (gint)gowl_client_get_pid(c);
	gchar *target;

	if (pid <= 0)
		return g_strdup("");
	link = g_strdup_printf("/proc/%d/exe", pid);
	target = g_file_read_link(link, NULL);
	return target != NULL ? target : g_strdup("");
}

static void
set_field(
	GHashTable  *f,
	const gchar *key,
	const gchar *value
){
	g_hash_table_replace(f, g_strdup(key), g_strdup(value != NULL ? value : ""));
}

/*
 * The facts a filter can name, for window @c (NULL: none).  The same
 * fields gabbrev's filters see, plus `id' and the key `mode'.
 */
GHashTable *
hk_fields_of(
	GowlModuleHotkey *self,
	GowlClient       *c
){
	static const gchar * const days[] = {
		"", "mon", "tue", "wed", "thu", "fri", "sat", "sun"
	};
	GHashTable *f;
	g_autoptr(GDateTime) now = g_date_time_new_now_local();
	GowlMonitor *mon = NULL;

	f = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	if (c != NULL) {
		g_autofree gchar *exec = exec_of(c);

		set_field(f, "app-id", gowl_client_get_app_id(c));
		set_field(f, "class", gowl_client_get_app_id(c));
		set_field(f, "title", gowl_client_get_title(c));
		set_field(f, "exec", exec);
		g_hash_table_replace(f, g_strdup("pid"),
		                     g_strdup_printf("%d", (gint)gowl_client_get_pid(c)));
		g_hash_table_replace(f, g_strdup("id"),
		                     g_strdup_printf("%u", gowl_client_get_id(c)));
		set_field(f, "xwayland", gowl_client_get_xwayland(c) ? "true" : "false");
		set_field(f, "floating", gowl_client_get_floating(c) ? "true" : "false");
		set_field(f, "fullscreen", gowl_client_get_fullscreen(c)
		          ? "true" : "false");
		mon = gowl_client_get_monitor(c);
	}
	if (mon == NULL && self->compositor != NULL)
		mon = gowl_compositor_get_selected_monitor(self->compositor);
	if (mon != NULL) {
		guint32 mask = gowl_monitor_get_tags(mon);
		GString *tags = g_string_new(NULL);
		gint lowest = 0;
		guint i;

		set_field(f, "monitor", gowl_monitor_get_name(mon));
		set_field(f, "layout", gowl_monitor_get_layout_symbol(mon));
		for (i = 0; i < 32; i++) {
			if ((mask & (1u << i)) == 0)
				continue;
			if (lowest == 0)
				lowest = (gint)i + 1;
			g_string_append_printf(tags, "%s%u", tags->len ? "," : "", i + 1);
		}
		g_hash_table_replace(f, g_strdup("tags"), g_string_free(tags, FALSE));
		g_hash_table_replace(f, g_strdup("tag"), g_strdup_printf("%d", lowest));
	}
	g_hash_table_replace(f, g_strdup("time"), g_date_time_format(now, "%H:%M"));
	g_hash_table_replace(f, g_strdup("hour"),
	                     g_strdup_printf("%d", g_date_time_get_hour(now)));
	set_field(f, "weekday", days[g_date_time_get_day_of_week(now)]);
	set_field(f, "mode", self->compositor != NULL
	          ? gowl_compositor_get_key_mode(self->compositor) : "default");
	return f;
}

/*
 * The first window (after @after, cycling) that @filter_text matches,
 * in the compositor's window order.
 */
GowlClient *
hk_find_window(
	GowlModuleHotkey *self,
	const gchar      *filter_text,
	GowlClient       *after,
	GError          **error
){
	g_autoptr(GowlFilter) f = NULL;
	GList *list;
	GList *l;
	GowlClient *first = NULL;
	gboolean passed = after == NULL;

	if (self->compositor == NULL)
		return NULL;
	f = gowl_filter_parse(filter_text, hk_window_fields(), error);
	if (f == NULL)
		return NULL;
	list = gowl_compositor_get_clients(self->compositor);
	for (l = list; l != NULL; l = l->next) {
		GowlClient *c = GOWL_CLIENT(l->data);
		g_autoptr(GHashTable) fields = NULL;

		if (gowl_client_get_wlr_surface(c) == NULL)
			continue;
		if (c == after) {
			passed = TRUE;
			continue;
		}
		fields = hk_fields_of(self, c);
		if (!gowl_filter_eval(f, fields))
			continue;
		if (first == NULL)
			first = c;
		if (passed)
			return c;
	}
	return first;
}

/* ── Conditions ──────────────────────────────────────────────────────── */

static GowlClient *
under_pointer(GowlModuleHotkey *self)
{
	gdouble x = 0;
	gdouble y = 0;

	if (self->compositor == NULL
	    || !gowl_compositor_get_cursor_position(self->compositor, &x, &y))
		return NULL;
	return gowl_compositor_client_at(self->compositor, x, y);
}

static gboolean
any_window(
	GowlModuleHotkey *self,
	const GowlFilter *f
){
	GList *l;

	if (self->compositor == NULL)
		return FALSE;
	for (l = gowl_compositor_get_clients(self->compositor); l != NULL;
	     l = l->next) {
		g_autoptr(GHashTable) fields = NULL;

		if (gowl_client_get_wlr_surface(GOWL_CLIENT(l->data)) == NULL)
			continue;
		fields = hk_fields_of(self, GOWL_CLIENT(l->data));
		if (gowl_filter_eval(f, fields))
			return TRUE;
	}
	return FALSE;
}

/* Does @cond hold now?  The focused window's fields are computed once
 * per input event (ev_fields). */
static gboolean
condition_holds(
	GowlModuleHotkey  *self,
	const HkCondition *cond,
	GowlClient        *focused
){
	if (cond->when != NULL) {
		if (self->ev_fields == NULL || self->ev_focused != focused) {
			g_clear_pointer(&self->ev_fields, g_hash_table_unref);
			self->ev_fields = hk_fields_of(self, focused);
			self->ev_focused = focused;
		}
		if (!gowl_filter_eval(cond->when, self->ev_fields))
			return FALSE;
	}
	if (cond->over != NULL) {
		g_autoptr(GHashTable) fields = hk_fields_of(self, under_pointer(self));

		if (!gowl_filter_eval(cond->over, fields))
			return FALSE;
	}
	if (cond->exists != NULL && !any_window(self, cond->exists))
		return FALSE;
	if (cond->absent != NULL && any_window(self, cond->absent))
		return FALSE;
	return TRUE;
}

/* A named context: the set's `contexts:', else a C predicate or a
 * boolean set from outside (cmacs's Elisp contexts). */
static gboolean
context_holds(
	GowlModuleHotkey *self,
	const gchar      *name,
	const gchar      *id,
	GowlClient       *focused
){
	HkNamedCondition *nc = NULL;
	GowlHotkeyCall *call;
	GError *e = NULL;
	gint v;

	if (self->set != NULL)
		nc = g_hash_table_lookup(self->set->contexts, name);
	if (nc != NULL)
		return condition_holds(self, &nc->cond, focused);
	call = gowl_hotkey_call_new(self->compositor, focused, id, "context",
	                            name, NULL);
	v = gowl_hotkey_eval_context(name, call, self->c_timeout_ms, &e);
	gowl_hotkey_call_free(call);
	if (e != NULL) {
		hk_fail(self, "%s", e->message);
		g_error_free(e);
	}
	return v == 1;
}

/* The engine's question: may @hk fire now? */
static gboolean
allow_hotkey(
	HkHotkey *hk,
	gpointer  data
){
	GowlModuleHotkey *self = data;
	GowlClient *focused;

	if (hk->max_running > 0 && !hk->is_remap
	    && hk_running_count(self, hk->id) >= hk->max_running)
		return FALSE;
	if (hk->context == NULL && hk_condition_is_empty(&hk->cond))
		return TRUE;
	focused = self->compositor != NULL
	          ? gowl_compositor_get_focused_client(self->compositor) : NULL;
	if (hk->context != NULL
	    && !context_holds(self, hk->context, hk->id, focused))
		return FALSE;
	return condition_holds(self, &hk->cond, focused);
}

/* ── Loading ─────────────────────────────────────────────────────────── */

static void timers_restart (GowlModuleHotkey *self);

/* The hotstrings, handed to gabbrev (or taken back from it). */
static void
install_hotstrings(GowlModuleHotkey *self)
{
	g_autofree gchar *yaml = NULL;
	GError *e = NULL;

	g_ptr_array_set_size(self->warnings, 0);
	if (self->hotstrings && self->set != NULL)
		yaml = hk_hotstrings_to_gabbrev(self->set->hotstrings, self->set,
		                                self->warnings);
	if (yaml == NULL) {
		if (self->hotstrings_installed)
			gowl_gabbrev_remove_source("hotkey");
		self->hotstrings_installed = FALSE;
		return;
	}
	if (!gowl_gabbrev_add_yaml_source("hotkey", yaml, &e)) {
		hk_fail(self, "the hotstrings did not make a gabbrev match file: %s",
		        e->message);
		g_error_free(e);
		return;
	}
	self->hotstrings_installed = TRUE;
}

/*
 * Every source into a fresh set, in order -- the config's `hotkey:'
 * sections, what code added through libgowl (a C config, a script,
 * cmacs), what was added at run time -- then into the engine.  Held
 * keys keep the hotkeys they were decided with.
 */
void
hk_reload(GowlModuleHotkey *self)
{
	g_autoptr(HkLoader) loader = hk_loader_new();
	g_autoptr(GPtrArray) code = NULL;
	GHashTableIter it;
	gpointer k;
	gpointer v;
	guint i;

	self->generation = gowl_hotkey_get_generation();
	/* C actions are built again on their next run (an edited file, a
	 * build held back for a fault, get a fresh chance) */
	g_hash_table_remove_all(self->c_funcs);
	g_hash_table_remove_all(self->c_faulted);
	if (self->config != NULL) {
		GPtrArray *nodes = gowl_config_get_hotkey_nodes(self->config);
		const gchar *base = gowl_config_get_hotkey_base_dir(self->config);

		for (i = 0; nodes != NULL && i < nodes->len; i++)
			hk_loader_add_node(loader, g_ptr_array_index(nodes, i), "config",
			                   base);
	}
	code = gowl_hotkey_dup_sources();
	for (i = 0; i < code->len; i++) {
		GowlHotkeySource *s = g_ptr_array_index(code, i);
		GError *e = NULL;

		if (!hk_loader_add_text(loader, s->yaml, s->id, &e)) {
			hk_fail(self, "source %s: %s", s->id, e->message);
			g_error_free(e);
		}
	}
	for (i = 0; i < self->runtime->len; i++) {
		GowlHotkeySource *s = g_ptr_array_index(self->runtime, i);
		GError *e = NULL;

		if (!hk_loader_add_text(loader, s->yaml, s->id, &e)) {
			hk_fail(self, "%s: %s", s->id, e->message);
			g_error_free(e);
		}
	}
	if (self->set != NULL)
		hk_set_unref(self->set);
	self->set = hk_loader_finish(loader);

	/* hotkey-enable/-disable outlive a reload */
	g_hash_table_iter_init(&it, self->enabled);
	while (g_hash_table_iter_next(&it, &k, &v)) {
		HkHotkey *hk = hk_set_find_hotkey(self->set, k);

		if (hk != NULL)
			hk->enabled = GPOINTER_TO_INT(v) != 0;
	}
	hk_engine_set_hotkeys(self->engine, self->set->hotkeys);
	install_hotstrings(self);
	if (self->started)
		timers_restart(self);
	if (self->set->problems->len > 0)
		hk_log(self, "%u problem(s) in the configuration; the first: %s "
		       "(hotkey-problems lists them)", self->set->problems->len,
		       (const gchar *)g_ptr_array_index(self->set->problems, 0));
	hk_log(self, "%u hotkey(s), %u hotstring(s), %u timer(s), %u hook(s)",
	       self->set->hotkeys->len, self->set->hotstrings->len,
	       self->set->timers->len, self->set->hooks->len);
	if (self->started)
		hk_scripts_sync(self);
}

/* libgowl's registries moved (a C config, a script, cmacs added
 * something): load again before deciding anything with stale data. */
static void
sync_registries(GowlModuleHotkey *self)
{
	if (self->generation != gowl_hotkey_get_generation())
		hk_reload(self);
}

static void on_config_reloaded (GowlConfig *config, gpointer data);

/* The config the compositor uses now; a reload may replace the object,
 * and the ref on the one loaded rules out a reused address. */
static void
sync_config(GowlModuleHotkey *self)
{
	GowlConfig *current;

	if (self->compositor == NULL)
		return;
	current = gowl_compositor_get_config(self->compositor);
	if (current == self->config)
		return;
	if (self->config != NULL) {
		if (self->reloaded_handler != 0)
			g_signal_handler_disconnect(self->config, self->reloaded_handler);
		self->reloaded_handler = 0;
		g_clear_object(&self->config);
	}
	if (current != NULL) {
		self->config = g_object_ref(current);
		self->reloaded_handler = g_signal_connect(current, "reloaded",
			G_CALLBACK(on_config_reloaded), self);
	}
	hk_reload(self);
}

static void
on_config_reloaded(
	GowlConfig *config,
	gpointer    data
){
	(void)config;
	hk_reload(GOWL_MODULE_HOTKEY(data));
}

/* ── Locks ───────────────────────────────────────────────────────────── */

/* Puts the locks back after the key's own modifier update has gone
 * through the keyboard group -- which is after filter_key returns. */
static void
restore_locks_idle(gpointer data)
{
	GowlModuleHotkey *self = data;
	gint caps = self->restore_caps;
	gint num = self->restore_num;

	self->restore_idle = NULL;
	self->restore_caps = -1;
	self->restore_num = -1;
	if (self->caps_force == HK_TOGGLE_ALWAYS_ON)
		caps = 1;
	else if (self->caps_force == HK_TOGGLE_ALWAYS_OFF)
		caps = 0;
	if (self->num_force == HK_TOGGLE_ALWAYS_ON)
		num = 1;
	else if (self->num_force == HK_TOGGLE_ALWAYS_OFF)
		num = 0;
	if (self->compositor != NULL && (caps >= 0 || num >= 0))
		gowl_compositor_set_lock_state(self->compositor, caps, num);
}

static void
schedule_lock_restore(
	GowlModuleHotkey *self,
	gint              caps,
	gint              num
){
	if (caps >= 0)
		self->restore_caps = caps;
	if (num >= 0)
		self->restore_num = num;
	if (self->restore_idle == NULL && self->compositor != NULL)
		self->restore_idle = wl_event_loop_add_idle(
			gowl_compositor_get_event_loop(self->compositor),
			restore_locks_idle, self);
}

/*
 * AutoHotkey's SetCapsLockState / SetNumLockState: on, off, toggle, or
 * forced -- always-on and always-off keep it so whatever is pressed,
 * normal stops forcing.
 */
void
hk_set_lock(
	GowlModuleHotkey *self,
	gboolean          caps,
	HkToggle          how
){
	gboolean cur_caps = FALSE;
	gboolean cur_num = FALSE;
	gint want;

	if (self->compositor == NULL)
		return;
	gowl_compositor_get_lock_state(self->compositor, &cur_caps, &cur_num);
	switch (how) {
	case HK_TOGGLE_ALWAYS_ON:
	case HK_TOGGLE_ALWAYS_OFF:
		if (caps)
			self->caps_force = how;
		else
			self->num_force = how;
		want = how == HK_TOGGLE_ALWAYS_ON ? 1 : 0;
		break;
	case HK_TOGGLE_NORMAL:
		if (caps)
			self->caps_force = HK_TOGGLE_NORMAL;
		else
			self->num_force = HK_TOGGLE_NORMAL;
		return;
	case HK_TOGGLE_TOGGLE:
		want = caps ? !cur_caps : !cur_num;
		break;
	case HK_TOGGLE_ON:
		want = 1;
		break;
	case HK_TOGGLE_OFF:
	default:
		want = 0;
		break;
	}
	gowl_compositor_set_lock_state(self->compositor, caps ? want : -1,
	                               caps ? -1 : want);
}

/* ── Modifier overlay ────────────────────────────────────────────────── */

/* The xkb mask of #GowlKeyMod bits @mods in @kb's keymap. */
static guint32
xkb_mask_of(
	struct wlr_keyboard *kb,
	guint                mods
){
	static const struct { guint mod; const gchar *name; } names[] = {
		{ GOWL_KEY_MOD_SHIFT, XKB_MOD_NAME_SHIFT },
		{ GOWL_KEY_MOD_CTRL,  XKB_MOD_NAME_CTRL },
		{ GOWL_KEY_MOD_ALT,   XKB_MOD_NAME_ALT },
		{ GOWL_KEY_MOD_LOGO,  XKB_MOD_NAME_LOGO },
		{ GOWL_KEY_MOD_MOD3,  "Mod3" },
		{ GOWL_KEY_MOD_MOD5,  "Mod5" }
	};
	guint32 mask = 0;
	guint i;

	if (kb->keymap == NULL)
		return 0;
	for (i = 0; i < G_N_ELEMENTS(names); i++) {
		xkb_mod_index_t idx;

		if (!(mods & names[i].mod))
			continue;
		idx = xkb_keymap_mod_get_index(kb->keymap, names[i].name);
		if (idx != XKB_MOD_INVALID)
			mask |= 1u << idx;
	}
	return mask;
}

static guint
overlay_mods(GowlModuleHotkey *self)
{
	guint mods = 0;
	guint i;

	for (i = 0; i < 8; i++)
		if (self->overlay_count[i] > 0)
			mods |= 1u << i;
	return mods;
}

/* The group's modifiers changed (a device's copied onto it): every
 * modifier a remap holds goes back in. */
static void
on_group_modifiers(
	struct wl_listener *listener,
	void               *data
){
	GowlModuleHotkey *self = wl_container_of(listener, self, group_mods);
	struct wlr_keyboard *kb;
	guint32 mask;

	(void)data;
	if (self->in_overlay || self->compositor == NULL
	    || self->compositor->wlr_kb_group == NULL)
		return;
	kb = &self->compositor->wlr_kb_group->keyboard;
	mask = xkb_mask_of(kb, overlay_mods(self));
	if (mask == 0 || (kb->modifiers.depressed & mask) == mask)
		return;
	self->in_overlay = TRUE;
	wlr_keyboard_notify_modifiers(kb, kb->modifiers.depressed | mask,
	                              kb->modifiers.latched,
	                              kb->modifiers.locked,
	                              kb->modifiers.group);
	self->in_overlay = FALSE;
}

/*
 * A remap pressed (or released) modifier @modbit.  The listener is on
 * the group keyboard only while something is held: wlroots aborts when
 * an object is destroyed with a listener still on it.
 */
void
hk_overlay_hold(
	GowlModuleHotkey *self,
	guint             modbit,
	gboolean          down
){
	guint i;

	for (i = 0; i < 8; i++) {
		if (modbit != (1u << i))
			continue;
		if (down)
			self->overlay_count[i]++;
		else if (self->overlay_count[i] > 0)
			self->overlay_count[i]--;
	}
	if (overlay_mods(self) == 0) {
		hk_overlay_detach(self);
		return;
	}
	if (!self->group_mods_attached && self->compositor != NULL
	    && self->compositor->wlr_kb_group != NULL) {
		self->group_mods.notify = on_group_modifiers;
		wl_signal_add(&self->compositor->wlr_kb_group->keyboard.events.modifiers,
		              &self->group_mods);
		self->group_mods_attached = TRUE;
	}
	on_group_modifiers(&self->group_mods, NULL);
}

void
hk_overlay_detach(GowlModuleHotkey *self)
{
	memset(self->overlay_count, 0, sizeof self->overlay_count);
	if (!self->group_mods_attached)
		return;
	wl_list_remove(&self->group_mods.link);
	wl_list_init(&self->group_mods.link);
	self->group_mods_attached = FALSE;
}

/* The compositor is being finalized: nothing may stay on its group. */
static void
on_compositor_gone(
	gpointer  data,
	GObject  *where
){
	GowlModuleHotkey *self = data;

	(void)where;
	if (self->group_mods_attached) {
		wl_list_remove(&self->group_mods.link);
		wl_list_init(&self->group_mods.link);
		self->group_mods_attached = FALSE;
	}
}

/* ── Keys ────────────────────────────────────────────────────────────── */

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

void
hk_set_suspended(
	GowlModuleHotkey *self,
	gboolean          suspended,
	const gchar      *why
){
	if (hk_engine_get_suspended(self->engine) == suspended)
		return;
	hk_engine_set_suspended(self->engine, suspended);
	hk_announce(self, suspended ? "Hotkeys suspended" : "Hotkeys on",
	            "suspended %s %s", suspended ? "on" : "off", why);
	if (self->dbus_service != NULL)
		hk_dbus_emit(self->dbus_service, "Suspended",
		             suspended ? "on" : "off", why);
}

/* The engine's swallow switch follows the mode in force. */
static void
sync_swallow(GowlModuleHotkey *self)
{
	const gchar *mode;
	HkMode *md = NULL;

	mode = self->compositor != NULL
	       ? gowl_compositor_get_key_mode(self->compositor) : "default";
	if (self->set != NULL && g_strcmp0(mode, "default") != 0)
		md = g_hash_table_lookup(self->set->modes, mode);
	hk_engine_set_swallow(self->engine, md != NULL && md->swallow);
}

/* One engine decision, with what follows it carried out. */
static gboolean
decide(
	GowlModuleHotkey *self,
	const HkEvent    *ev
){
	g_autoptr(GArray) out = hk_out_new();
	gboolean consume;
	const gchar *mode;

	sync_swallow(self);
	mode = self->compositor != NULL
	       ? gowl_compositor_get_key_mode(self->compositor) : "default";
	consume = hk_engine_input(self->engine, ev, mode, now_ms(), out);
	g_clear_pointer(&self->ev_fields, g_hash_table_unref);
	self->ev_focused = NULL;
	hk_play_outputs(self, out);
	hk_out_clear(out);
	hk_arm_deadline(self);
	return consume;
}

static gboolean
hk_filter_key(
	GowlInputObserver          *observer,
	const GowlInputObserverKey *key
){
	GowlModuleHotkey *self = GOWL_MODULE_HOTKEY(observer);
	gpointer kc = GUINT_TO_POINTER(key->keycode);
	gboolean own = self->injecting > 0;
	gboolean is_caps;
	gboolean is_num;
	gboolean consume;
	HkEvent ev;

	if (!self->active || self->compositor == NULL || self->set == NULL)
		return FALSE;
	sync_registries(self);

	/* The release of a press the module itself took */
	if (!key->pressed && g_hash_table_remove(self->swallowed, kc))
		return TRUE;

	is_caps = key->base_keysym == XKB_KEY_Caps_Lock;
	is_num = key->base_keysym == XKB_KEY_Num_Lock;

	/* The suspend key, working while suspended (it is exempt) and while
	 * a game inhibits shortcuts */
	if (!own && key->pressed && key_is(key, self->suspend_mods,
	                                   self->suspend_sym)) {
		g_hash_table_add(self->swallowed, kc);
		hk_set_suspended(self, !hk_engine_get_suspended(self->engine), "key");
		return TRUE;
	}

	/* An input hook collecting keys */
	if (!own && self->input.active
	    && hk_input_key(self, key->keycode, key->keysym, key->base_keysym,
	                    key->modifiers, key->pressed))
		return TRUE;

	/* A forced lock: the key that would change it changes nothing */
	if (!own && ((is_caps && self->caps_force != HK_TOGGLE_NORMAL)
	             || (is_num && self->num_force != HK_TOGGLE_NORMAL))) {
		if (key->pressed)
			g_hash_table_add(self->swallowed, kc);
		schedule_lock_restore(self, -1, -1);
		return TRUE;
	}
	/* What the locks were before this key, to put back if the engine
	 * swallows a Caps Lock -- the keyboard toggled it already */
	if (!is_caps && !is_num)
		gowl_compositor_get_lock_state(self->compositor, &self->caps_before,
		                               &self->num_before);

	memset(&ev, 0, sizeof ev);
	ev.kind = GOWL_HOTKEY_INPUT_KEY;
	ev.keycode = key->keycode;
	ev.keysym = key->keysym;
	ev.base_keysym = key->base_keysym;
	ev.mods = key->modifiers;
	ev.pressed = key->pressed;
	ev.synthetic = key->synthetic && !own;
	ev.own_level = own ? self->inject_level : -1;
	ev.focused = key->focused;
	consume = decide(self, &ev);
	if (consume && !own && (is_caps || is_num))
		schedule_lock_restore(self, is_caps ? (gint)self->caps_before : -1,
		                      is_num ? (gint)self->num_before : -1);
	return consume;
}

static void
hk_observer_init(GowlInputObserverInterface *iface)
{
	iface->filter_key = hk_filter_key;
}

/* ── Buttons and the wheel ───────────────────────────────────────────── */

static gboolean
hk_handle_button(
	GowlMouseHandler *handler,
	guint             button,
	guint             state,
	guint             modifiers
){
	GowlModuleHotkey *self = GOWL_MODULE_HOTKEY(handler);
	gboolean own = self->injecting > 0;
	HkEvent ev;

	if (!self->active || self->compositor == NULL || self->set == NULL)
		return FALSE;
	sync_registries(self);
	if (!own && self->input.active && state != 0)
		/* A click ends an input hook, as it resets a hotstring */
		hk_input_stop(self, "mouse");
	memset(&ev, 0, sizeof ev);
	ev.kind = GOWL_HOTKEY_INPUT_BUTTON;
	ev.button = button;
	ev.mods = GOWL_CLEANMASK(modifiers);
	ev.pressed = state != 0;
	ev.own_level = own ? self->inject_level : -1;
	ev.focused = gowl_compositor_get_focused_client(self->compositor);
	return decide(self, &ev);
}

/* A wheel step: one notch per discrete step, or per 15 units of a
 * smooth scroll (a touchpad) -- each notch is one hotkey press. */
static gboolean
hk_handle_axis(
	GowlMouseHandler *handler,
	guint             axis,
	gdouble           delta,
	gint              discrete,
	guint             modifiers
){
	GowlModuleHotkey *self = GOWL_MODULE_HOTKEY(handler);
	gboolean own = self->injecting > 0;
	gboolean consume = FALSE;
	gint notches;
	gint i;
	HkEvent ev;

	if (!self->active || self->compositor == NULL || self->set == NULL
	    || axis > 1)
		return FALSE;
	sync_registries(self);
	if (discrete != 0) {
		/* v120 or whole clicks, whichever the source gave */
		notches = (ABS(discrete) >= 120) ? discrete / 120 : discrete;
		if (notches == 0)
			notches = discrete > 0 ? 1 : -1;
	} else {
		self->wheel_accum[axis] += delta;
		notches = (gint)(self->wheel_accum[axis] / 15.0);
		self->wheel_accum[axis] -= notches * 15.0;
		if (notches == 0)
			return FALSE;
	}
	memset(&ev, 0, sizeof ev);
	ev.kind = GOWL_HOTKEY_INPUT_WHEEL;
	ev.pressed = TRUE;
	ev.mods = GOWL_CLEANMASK(modifiers);
	ev.own_level = own ? self->inject_level : -1;
	ev.focused = gowl_compositor_get_focused_client(self->compositor);
	if (axis == 0)
		ev.button = notches < 0 ? GOWL_BUTTON_WHEEL_UP : GOWL_BUTTON_WHEEL_DOWN;
	else
		ev.button = notches < 0 ? GOWL_BUTTON_WHEEL_LEFT
		                        : GOWL_BUTTON_WHEEL_RIGHT;
	for (i = 0; i < ABS(notches) && i < 20; i++)
		consume = decide(self, &ev) || consume;
	return consume;
}

static void
hk_mouse_init(GowlMouseHandlerInterface *iface)
{
	iface->handle_button = hk_handle_button;
	iface->handle_axis = hk_handle_axis;
}

/* ── Time ────────────────────────────────────────────────────────────── */

static gint
deadline_tick(gpointer data)
{
	GowlModuleHotkey *self = data;
	g_autoptr(GArray) out = hk_out_new();

	hk_engine_tick(self->engine, now_ms(), out);
	hk_play_outputs(self, out);
	hk_out_clear(out);
	hk_arm_deadline(self);
	return 0;
}

/* The engine's next long press or double tap to decide. */
void
hk_arm_deadline(GowlModuleHotkey *self)
{
	gint64 next;
	gint64 wait;

	if (self->compositor == NULL)
		return;
	next = hk_engine_next_deadline(self->engine);
	if (next < 0) {
		if (self->deadline_timer != NULL)
			wl_event_source_timer_update(self->deadline_timer, 0);
		return;
	}
	if (self->deadline_timer == NULL)
		self->deadline_timer = wl_event_loop_add_timer(
			gowl_compositor_get_event_loop(self->compositor),
			deadline_tick, self);
	wait = next - now_ms();
	wl_event_source_timer_update(self->deadline_timer,
	                             (gint)CLAMP(wait, 1, G_MAXINT));
}

/* ── Timers (`timers:') ──────────────────────────────────────────────── */

static void
timer_run_free(gpointer p)
{
	HkTimerRun *t = p;

	if (t->source != NULL)
		wl_event_source_remove(t->source);
	g_free(t->id);
	g_free(t);
}

static gint
timer_fire(gpointer data)
{
	HkTimerRun *t = data;
	GowlModuleHotkey *self = t->self;
	g_autofree gchar *detail = NULL;
	GowlClient *focused;

	if (!t->enabled || !self->active)
		return 0;
	if (!t->def->once)
		wl_event_source_timer_update(t->source, (gint)t->def->period_ms);
	if (hk_engine_get_suspended(self->engine))
		return 0;
	focused = gowl_compositor_get_focused_client(self->compositor);
	if (t->def->context != NULL
	    && !context_holds(self, t->def->context, t->id, focused))
		return 0;
	if (!condition_holds(self, &t->def->cond, focused)) {
		g_clear_pointer(&self->ev_fields, g_hash_table_unref);
		return 0;
	}
	g_clear_pointer(&self->ev_fields, g_hash_table_unref);
	t->fired++;
	detail = g_strdup_printf("%u", t->def->period_ms);
	if (hk_running_count(self, t->id) == 0)
		hk_run_actions(self, t->def->actions, t->id, "timer", detail, NULL,
		               TRUE);
	return 0;
}

static void
timers_restart(GowlModuleHotkey *self)
{
	GHashTable *was_disabled;
	guint i;

	if (self->compositor == NULL)
		return;
	/* A timer switched off by hand stays off across a reload */
	was_disabled = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	for (i = 0; i < self->timers->len; i++) {
		HkTimerRun *t = g_ptr_array_index(self->timers, i);

		if (!t->enabled)
			g_hash_table_add(was_disabled, g_strdup(t->id));
	}
	g_ptr_array_set_size(self->timers, 0);
	for (i = 0; self->set != NULL && i < self->set->timers->len; i++) {
		HkTimer *def = g_ptr_array_index(self->set->timers, i);
		HkTimerRun *t = g_new0(HkTimerRun, 1);

		t->self = self;
		t->def = def;
		t->id = g_strdup(def->id);
		t->enabled = def->enabled && !g_hash_table_contains(was_disabled,
		                                                    def->id);
		t->source = wl_event_loop_add_timer(
			gowl_compositor_get_event_loop(self->compositor), timer_fire, t);
		if (t->enabled)
			wl_event_source_timer_update(t->source,
			                             (gint)MAX(def->delay_ms, 1));
		g_ptr_array_add(self->timers, t);
	}
	g_hash_table_unref(was_disabled);
}

static HkTimerRun *
find_timer(
	GowlModuleHotkey *self,
	const gchar      *id
){
	guint i;

	for (i = 0; i < self->timers->len; i++) {
		HkTimerRun *t = g_ptr_array_index(self->timers, i);

		if (g_strcmp0(t->id, id) == 0)
			return t;
	}
	return NULL;
}

/* ── Event hooks (`on:') ─────────────────────────────────────────────── */

static void
run_hooks(
	GowlModuleHotkey *self,
	const gchar      *event,
	GowlClient       *window,
	const gchar      *detail
){
	guint i;

	if (!self->active || self->set == NULL
	    || hk_engine_get_suspended(self->engine))
		return;
	for (i = 0; i < self->set->hooks->len; i++) {
		HkHook *h = g_ptr_array_index(self->set->hooks, i);
		g_autofree gchar *d = NULL;

		if (g_strcmp0(h->event, event) != 0)
			continue;
		/* `filter:'/`when:' is about the event's window */
		if (!condition_holds(self, &h->cond, window)) {
			g_clear_pointer(&self->ev_fields, g_hash_table_unref);
			continue;
		}
		g_clear_pointer(&self->ev_fields, g_hash_table_unref);
		d = g_strdup_printf("%s %u %s", event,
		                    window != NULL ? gowl_client_get_id(window) : 0,
		                    detail != NULL ? detail : "");
		hk_run_actions(self, h->actions, h->id, "event", g_strstrip(d), NULL,
		               TRUE);
	}
}

static void
on_focus_changed(
	GowlCompositor *compositor,
	GObject        *client,
	gpointer        data
){
	(void)compositor;
	if (client != NULL && GOWL_IS_CLIENT(client))
		run_hooks(GOWL_MODULE_HOTKEY(data), "focus", GOWL_CLIENT(client), NULL);
}

static void
on_client_added(
	GowlCompositor *compositor,
	GObject        *client,
	gpointer        data
){
	(void)compositor;
	if (client != NULL && GOWL_IS_CLIENT(client))
		run_hooks(GOWL_MODULE_HOTKEY(data), "window-open", GOWL_CLIENT(client),
		          NULL);
}

static void
on_client_removed(
	GowlCompositor *compositor,
	GObject        *client,
	gpointer        data
){
	(void)compositor;
	if (client != NULL && GOWL_IS_CLIENT(client))
		run_hooks(GOWL_MODULE_HOTKEY(data), "window-close",
		          GOWL_CLIENT(client), NULL);
}

static void
on_title_changed(
	GowlCompositor *compositor,
	GowlClient     *client,
	gpointer        data
){
	(void)compositor;
	run_hooks(GOWL_MODULE_HOTKEY(data), "title", client, NULL);
}

static void
on_lock_changed(
	GowlCompositor *compositor,
	gboolean        locked,
	gpointer        data
){
	GowlModuleHotkey *self = GOWL_MODULE_HOTKEY(data);

	(void)compositor;
	if (locked) {
		/* A lock is no time to leave anything held or pending */
		g_autoptr(GArray) out = hk_out_new();

		hk_engine_reset(self->engine, out);
		hk_play_outputs(self, out);
		hk_out_clear(out);
		if (self->input.active)
			hk_input_stop(self, "lock");
	}
	run_hooks(self, locked ? "lock" : "unlock", NULL, NULL);
}

static void
on_mode_changed(
	GowlCompositor *compositor,
	const gchar    *mode,
	gpointer        data
){
	GowlModuleHotkey *self = GOWL_MODULE_HOTKEY(data);

	(void)compositor;
	hk_mode_changed(self, mode);
	run_hooks(self, "mode", NULL, mode);
}

static void
on_monitor_added(
	GowlCompositor *compositor,
	GowlMonitor    *monitor,
	gpointer        data
){
	(void)compositor;
	run_hooks(GOWL_MODULE_HOTKEY(data), "monitor", NULL,
	          monitor != NULL ? gowl_monitor_get_name(monitor) : NULL);
}

/* A tag switch: the monitor's tags after an arrange, against the last
 * seen (kept on the monitor object). */
static void
on_arranged(
	GowlCompositor *compositor,
	GowlMonitor    *monitor,
	gpointer        data
){
	guint32 tags;
	guint32 last;
	g_autofree gchar *d = NULL;

	(void)compositor;
	if (monitor == NULL)
		return;
	tags = gowl_monitor_get_tags(monitor);
	last = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(monitor),
	                                          "gowl-hotkey-tags"));
	if (last == tags)
		return;
	g_object_set_data(G_OBJECT(monitor), "gowl-hotkey-tags",
	                  GUINT_TO_POINTER(tags));
	if (last == 0)
		return;
	d = g_strdup_printf("%u %s", tags, gowl_monitor_get_name(monitor));
	run_hooks(GOWL_MODULE_HOTKEY(data), "tag", NULL, d);
}

/* ── IPC ─────────────────────────────────────────────────────────────── */

static gchar *
json_reply(JsonBuilder *b)
{
	g_autoptr(JsonGenerator) gen = json_generator_new();
	g_autoptr(JsonNode) root = json_builder_get_root(b);
	g_autofree gchar *text = NULL;

	json_generator_set_root(gen, root);
	text = json_generator_to_data(gen, NULL);
	return g_strdup_printf("OK %s", text);
}

static JsonNode *
status_json(GowlModuleHotkey *self)
{
	g_autoptr(JsonBuilder) b = json_builder_new();
	const gchar *prior;
	gint64 prior_ms = 0;
	gboolean caps = FALSE;
	gboolean num = FALSE;

	if (self->compositor != NULL)
		gowl_compositor_get_lock_state(self->compositor, &caps, &num);
	prior = hk_engine_get_prior(self->engine, &prior_ms);
	json_builder_begin_object(b);
	json_builder_set_member_name(b, "active");
	json_builder_add_boolean_value(b, self->active);
	json_builder_set_member_name(b, "suspended");
	json_builder_add_boolean_value(b, hk_engine_get_suspended(self->engine));
	json_builder_set_member_name(b, "suspend-key");
	json_builder_add_string_value(b, self->suspend_text);
	json_builder_set_member_name(b, "mode");
	json_builder_add_string_value(b, self->compositor != NULL
		? gowl_compositor_get_key_mode(self->compositor) : "default");
	json_builder_set_member_name(b, "hotkeys");
	json_builder_add_int_value(b, self->set != NULL ? self->set->hotkeys->len : 0);
	json_builder_set_member_name(b, "enabled");
	json_builder_add_int_value(b, self->set != NULL
		? hk_set_count_enabled(self->set) : 0);
	json_builder_set_member_name(b, "hotstrings");
	json_builder_add_int_value(b, self->set != NULL
		? self->set->hotstrings->len : 0);
	json_builder_set_member_name(b, "hotstrings-active");
	json_builder_add_boolean_value(b, self->hotstrings_installed
		&& self->compositor != NULL
		&& gowl_module_manager_find_module(
			gowl_compositor_get_module_manager(self->compositor),
			"gabbrev") != NULL);
	json_builder_set_member_name(b, "timers");
	json_builder_add_int_value(b, self->timers->len);
	json_builder_set_member_name(b, "hooks");
	json_builder_add_int_value(b, self->set != NULL ? self->set->hooks->len : 0);
	json_builder_set_member_name(b, "modes");
	json_builder_add_int_value(b, self->set != NULL
		? g_hash_table_size(self->set->modes) : 0);
	json_builder_set_member_name(b, "scripts");
	json_builder_add_int_value(b, g_hash_table_size(self->scripts));
	json_builder_set_member_name(b, "running");
	json_builder_add_int_value(b, self->runners->len);
	json_builder_set_member_name(b, "held");
	json_builder_add_int_value(b, hk_engine_n_held(self->engine));
	json_builder_set_member_name(b, "fired");
	json_builder_add_int_value(b, (gint64)self->n_fired);
	json_builder_set_member_name(b, "failed");
	json_builder_add_int_value(b, (gint64)self->n_failed);
	json_builder_set_member_name(b, "prior");
	json_builder_add_string_value(b, prior != NULL ? prior : "");
	json_builder_set_member_name(b, "capslock");
	json_builder_add_boolean_value(b, caps);
	json_builder_set_member_name(b, "numlock");
	json_builder_add_boolean_value(b, num);
	json_builder_set_member_name(b, "input-hook");
	json_builder_add_boolean_value(b, self->input.active);
	json_builder_set_member_name(b, "dbus");
	json_builder_add_boolean_value(b, self->dbus_service != NULL);
	json_builder_set_member_name(b, "problems");
	json_builder_add_int_value(b, self->set != NULL
		? self->set->problems->len : 0);
	json_builder_set_member_name(b, "warnings");
	json_builder_begin_array(b);
	{
		guint i;

		for (i = 0; i < self->warnings->len; i++)
			json_builder_add_string_value(b,
				g_ptr_array_index(self->warnings, i));
	}
	json_builder_end_array(b);
	if (self->hotstrings_installed && self->compositor != NULL
	    && gowl_module_manager_find_module(
		    gowl_compositor_get_module_manager(self->compositor),
		    "gabbrev") == NULL) {
		json_builder_set_member_name(b, "hotstrings-note");
		json_builder_add_string_value(b, "the gabbrev module is not loaded, "
		                              "so the hotstrings do nothing yet");
	}
	if (self->last_error != NULL) {
		json_builder_set_member_name(b, "last-error");
		json_builder_add_string_value(b, self->last_error);
	}
	json_builder_end_object(b);
	return json_builder_get_root(b);
}

static JsonNode *
list_json(GowlModuleHotkey *self)
{
	g_autoptr(JsonBuilder) b = json_builder_new();
	guint i;

	json_builder_begin_array(b);
	for (i = 0; self->set != NULL && i < self->set->hotkeys->len; i++)
		hk_hotkey_to_json(g_ptr_array_index(self->set->hotkeys, i), b);
	json_builder_end_array(b);
	return json_builder_get_root(b);
}

static gchar *
node_reply(JsonNode *node)
{
	g_autoptr(JsonGenerator) gen = json_generator_new();
	g_autofree gchar *text = NULL;

	json_generator_set_root(gen, node);
	text = json_generator_to_data(gen, NULL);
	json_node_unref(node);
	return g_strdup_printf("OK %s", text);
}

static gchar *
strv_reply(GStrv v)
{
	g_autoptr(JsonBuilder) b = json_builder_new();
	guint i;

	json_builder_begin_array(b);
	for (i = 0; v != NULL && v[i] != NULL; i++)
		json_builder_add_string_value(b, v[i]);
	json_builder_end_array(b);
	return json_reply(b);
}

static gchar *
cmd_history(GowlModuleHotkey *self)
{
	g_autoptr(JsonBuilder) b = json_builder_new();
	GPtrArray *h = hk_engine_get_history(self->engine);
	guint i;

	json_builder_begin_array(b);
	for (i = 0; i < h->len; i++) {
		HkHistoryEntry *e = g_ptr_array_index(h, i);
		gchar name[64];

		json_builder_begin_object(b);
		json_builder_set_member_name(b, "t");
		json_builder_add_int_value(b, e->time_ms);
		json_builder_set_member_name(b, "kind");
		json_builder_add_string_value(b,
			e->kind == GOWL_HOTKEY_INPUT_KEY ? "key"
			: e->kind == GOWL_HOTKEY_INPUT_BUTTON ? "button" : "wheel");
		json_builder_set_member_name(b, "code");
		json_builder_add_int_value(b, e->code);
		if (e->kind == GOWL_HOTKEY_INPUT_KEY
		    && xkb_keysym_get_name(e->keysym, name, sizeof name) > 0) {
			json_builder_set_member_name(b, "keysym");
			json_builder_add_string_value(b, name);
		}
		json_builder_set_member_name(b, "pressed");
		json_builder_add_boolean_value(b, e->pressed);
		json_builder_set_member_name(b, "synthetic");
		json_builder_add_boolean_value(b, e->synthetic);
		json_builder_set_member_name(b, "own");
		json_builder_add_boolean_value(b, e->own);
		json_builder_set_member_name(b, "consumed");
		json_builder_add_boolean_value(b, e->consumed);
		if (e->fired != NULL) {
			json_builder_set_member_name(b, "fired");
			json_builder_add_string_value(b, e->fired);
		}
		json_builder_end_object(b);
	}
	json_builder_end_array(b);
	return json_reply(b);
}

static gchar *
cmd_timers(GowlModuleHotkey *self)
{
	g_autoptr(JsonBuilder) b = json_builder_new();
	guint i;

	json_builder_begin_array(b);
	for (i = 0; i < self->timers->len; i++) {
		HkTimerRun *t = g_ptr_array_index(self->timers, i);

		json_builder_begin_object(b);
		json_builder_set_member_name(b, "id");
		json_builder_add_string_value(b, t->id);
		json_builder_set_member_name(b, "period-ms");
		json_builder_add_int_value(b, t->def->period_ms);
		json_builder_set_member_name(b, "once");
		json_builder_add_boolean_value(b, t->def->once);
		json_builder_set_member_name(b, "enabled");
		json_builder_add_boolean_value(b, t->enabled);
		json_builder_set_member_name(b, "fired");
		json_builder_add_int_value(b, t->fired);
		json_builder_end_object(b);
	}
	json_builder_end_array(b);
	return json_reply(b);
}

/* Whether hotkey-add's text is one hotkey entry rather than a section:
 * it names a `key' and none of the section keys that hold one.  Block
 * YAML (`key:') and JSON, which is YAML flow syntax (`"key":'), both. */
static gboolean
add_is_entry(const gchar *text)
{
	static const gchar * const section[] = {
		"hotkeys", "remaps", "modes", "timers", NULL
	};
	g_autofree gchar *quoted = NULL;
	g_autofree gchar *plain = NULL;
	guint i;

	if (strstr(text, "key:") == NULL && strstr(text, "\"key\":") == NULL)
		return FALSE;
	for (i = 0; section[i] != NULL; i++) {
		plain = g_strdup_printf("%s:", section[i]);
		quoted = g_strdup_printf("\"%s\":", section[i]);
		if (strstr(text, plain) != NULL || strstr(text, quoted) != NULL)
			return FALSE;
		g_clear_pointer(&plain, g_free);
		g_clear_pointer(&quoted, g_free);
	}
	return TRUE;
}

/*
 * hotkey-add: a YAML section -- or one hotkey entry, which is wrapped
 * as `hotkeys: [ENTRY]' -- for this session.  Checked here: an entry
 * with a problem is refused rather than half-added.
 *
 * `--id=ID' first names the source, so adding under the same id again
 * replaces it and `hotkey-remove ID' takes all of it back -- how an
 * embedder keeps a whole section of its own (cmacs's remaps and
 * hotstrings).  Without it a single hotkey is named by its own id and a
 * section gets `runtime-N'.
 */
static gchar *
cmd_add(
	GowlModuleHotkey *self,
	const gchar      *args
){
	g_autoptr(HkLoader) loader = hk_loader_new();
	g_autoptr(HkSet) check = NULL;
	g_autofree gchar *yaml = NULL;
	g_autofree gchar *id = NULL;
	g_autofree gchar *copy = g_strstrip(g_strdup(args != NULL ? args : ""));
	g_autofree gchar *given_id = NULL;
	GowlHotkeySource *src;
	GError *e = NULL;
	guint i;

	args = copy;
	/* --id=ID: the source's name */
	if (g_str_has_prefix(args, "--id=")) {
		const gchar *end = args + 5;

		while (*end != '\0' && !g_ascii_isspace(*end))
			end++;
		given_id = g_strndup(args + 5, (gsize)(end - (args + 5)));
		if (*given_id == '\0')
			return g_strdup("ERROR hotkey-add --id= needs a name");
		while (g_ascii_isspace(*end))
			end++;
		args = end;
	}
	if (*args == '\0')
		return g_strdup("ERROR hotkey-add needs YAML: {key: ..., do: ...} or "
		                "a section");
	/* One entry, or a section? */
	if (add_is_entry(args))
		yaml = g_strdup_printf("hotkeys:\n  - %s\n", args);
	else
		yaml = g_strdup(args);
	if (!hk_loader_add_text(loader, yaml, "hotkey-add", &e)) {
		gchar *r = g_strdup_printf("ERROR %s", e->message);

		g_error_free(e);
		return r;
	}
	check = hk_loader_finish(loader);
	for (i = 0; i < check->problems->len; i++) {
		const gchar *p = g_ptr_array_index(check->problems, i);

		/* An unknown mode is a note, not a refusal */
		if (strstr(p, "modes: does not describe") == NULL)
			return g_strdup_printf("ERROR %s", p);
	}
	if (check->hotkeys->len == 0 && check->timers->len == 0
	    && check->hotstrings->len == 0 && check->hooks->len == 0
	    && g_hash_table_size(check->functions) == 0
	    && g_hash_table_size(check->modes) == 0
	    && g_hash_table_size(check->contexts) == 0
	    && g_hash_table_size(check->groups) == 0)
		return g_strdup("ERROR nothing to add");
	id = given_id != NULL ? g_steal_pointer(&given_id)
	     : check->hotkeys->len == 1
	     ? g_strdup(((HkHotkey *)g_ptr_array_index(check->hotkeys, 0))->id)
	     : g_strdup_printf("runtime-%u", self->runtime->len + 1);
	/* The same id again replaces it */
	for (i = 0; i < self->runtime->len; i++) {
		GowlHotkeySource *s = g_ptr_array_index(self->runtime, i);

		if (g_strcmp0(s->id, id) == 0) {
			g_ptr_array_remove_index(self->runtime, i);
			break;
		}
	}
	src = g_new0(GowlHotkeySource, 1);
	src->id = g_strdup(id);
	src->yaml = g_steal_pointer(&yaml);
	g_ptr_array_add(self->runtime, src);
	hk_reload(self);
	return g_strdup_printf("OK added %s", id);
}

/* hotkey-remove: a hotkey added at run time, or any hotkey by id
 * (which is then disabled: what came from the config comes back on the
 * next config change). */
static gchar *
cmd_remove(
	GowlModuleHotkey *self,
	const gchar      *id
){
	guint i;

	for (i = 0; i < self->runtime->len; i++) {
		GowlHotkeySource *s = g_ptr_array_index(self->runtime, i);

		if (g_strcmp0(s->id, id) == 0) {
			g_ptr_array_remove_index(self->runtime, i);
			hk_reload(self);
			return g_strdup_printf("OK removed %s", id);
		}
	}
	if (gowl_hotkey_undefine(id)) {
		hk_reload(self);
		return g_strdup_printf("OK removed %s", id);
	}
	if (self->set != NULL && hk_set_find_hotkey(self->set, id) != NULL) {
		hk_set_hotkey_enabled(self, id, HK_TOGGLE_OFF);
		return g_strdup_printf("OK disabled %s (it is from the "
		                       "configuration)", id);
	}
	return g_strdup_printf("ERROR no hotkey %s", id);
}

gboolean
hk_set_hotkey_enabled(
	GowlModuleHotkey *self,
	const gchar      *id,
	HkToggle          how
){
	HkHotkey *hk;
	gboolean on;

	if (self->set == NULL || (hk = hk_set_find_hotkey(self->set, id)) == NULL)
		return FALSE;
	on = how == HK_TOGGLE_TOGGLE ? !hk->enabled : how == HK_TOGGLE_ON;
	hk->enabled = on;
	g_hash_table_replace(self->enabled, g_strdup(id), GINT_TO_POINTER(on));
	hk_announce(self, NULL, "%s %s", on ? "enabled" : "disabled", id);
	return TRUE;
}

static gboolean
parse_toggle_arg(
	const gchar *arg,
	HkToggle    *out
){
	if (arg == NULL || *arg == '\0' || g_ascii_strcasecmp(arg, "toggle") == 0)
		*out = HK_TOGGLE_TOGGLE;
	else if (g_ascii_strcasecmp(arg, "on") == 0
	         || g_strcmp0(arg, "1") == 0 || g_ascii_strcasecmp(arg, "true") == 0)
		*out = HK_TOGGLE_ON;
	else if (g_ascii_strcasecmp(arg, "off") == 0
	         || g_strcmp0(arg, "0") == 0 || g_ascii_strcasecmp(arg, "false") == 0)
		*out = HK_TOGGLE_OFF;
	else if (g_ascii_strcasecmp(arg, "always-on") == 0)
		*out = HK_TOGGLE_ALWAYS_ON;
	else if (g_ascii_strcasecmp(arg, "always-off") == 0)
		*out = HK_TOGGLE_ALWAYS_OFF;
	else if (g_ascii_strcasecmp(arg, "normal") == 0)
		*out = HK_TOGGLE_NORMAL;
	else
		return FALSE;
	return TRUE;
}

static gchar *
cmd_keystate(
	GowlModuleHotkey *self,
	const gchar      *name
){
	GowlHotkeyInput in;
	GError *e = NULL;
	gboolean caps = FALSE;
	gboolean num = FALSE;

	if (!gowl_hotkey_input_parse(name, &in, &e)) {
		gchar *r = g_strdup_printf("ERROR %s", e->message);

		g_error_free(e);
		return r;
	}
	/* The toggle state of a lock key, AutoHotkey's GetKeyState("T") */
	if (in.kind == GOWL_HOTKEY_INPUT_KEY && self->compositor != NULL
	    && (in.keysym == XKB_KEY_Caps_Lock || in.keysym == XKB_KEY_Num_Lock)) {
		gowl_compositor_get_lock_state(self->compositor, &caps, &num);
		return g_strdup_printf("OK {\"down\": %s, \"toggled\": %s}",
		                       hk_engine_key_is_down(self->engine, &in)
		                       ? "true" : "false",
		                       (in.keysym == XKB_KEY_Caps_Lock ? caps : num)
		                       ? "true" : "false");
	}
	return g_strdup_printf("OK {\"down\": %s}",
	                       hk_engine_key_is_down(self->engine, &in)
	                       ? "true" : "false");
}

static const gchar help_text[] =
	"OK hotkey-status | hotkey-list [json] | hotkey-add [--id=ID] YAML | "
	"hotkey-remove ID | hotkey-enable ID | hotkey-disable ID | "
	"hotkey-toggle ID | hotkey-suspend [on|off|toggle] | hotkey-mode [NAME] | "
	"hotkey-send KEYS | hotkey-send-text TEXT | hotkey-fire ID | "
	"hotkey-call NAME [ARGS] | hotkey-reload | hotkey-history | "
	"hotkey-keystate KEY | hotkey-capslock on|off|toggle|always-on|"
	"always-off|normal | hotkey-numlock ... | hotkey-context-set NAME 0|1 | "
	"hotkey-context-clear NAME | hotkey-contexts | "
	"hotkey-func-define NAME FORM | hotkey-func-undefine NAME | "
	"hotkey-functions | hotkey-input-start [NAME] [L5 T2000 V C] | "
	"hotkey-input-stop | hotkey-problems | hotkey-timers | "
	"hotkey-timer-enable ID | hotkey-timer-disable ID | hotkey-cancel | "
	"hotkey-help";

/* The first word of @args, and the rest (stripped), in place. */
static gchar *
split_word(
	gchar  *args,
	gchar **rest
){
	gchar *sp;

	args = g_strstrip(args);
	sp = strchr(args, ' ');
	if (sp == NULL) {
		*rest = args + strlen(args);
		return args;
	}
	*sp = '\0';
	*rest = g_strstrip(sp + 1);
	return args;
}

/*
 * hk_dispatch:
 * @self: the module
 * @verb: the command after `hotkey-'
 * @args: (nullable): the rest of the line
 *
 * Every text command, shared by the IPC handler, the JSON-RPC methods
 * and the D-Bus service.
 *
 * Returns: (transfer full): `OK ...' or `ERROR ...'
 */
gchar *
hk_dispatch(
	GowlModuleHotkey *self,
	const gchar      *verb,
	const gchar      *args
){
	g_autofree gchar *copy = g_strdup(args != NULL ? args : "");
	gchar *arg = g_strstrip(copy);
	HkToggle t;

	if (g_strcmp0(verb, "help") == 0)
		return g_strdup(help_text);
	if (g_strcmp0(verb, "status") == 0)
		return node_reply(status_json(self));
	if (!self->active)
		return g_strdup("ERROR the hotkey module is not active");
	sync_registries(self);

	if (g_strcmp0(verb, "list") == 0)
		return node_reply(list_json(self));
	if (g_strcmp0(verb, "problems") == 0) {
		g_autoptr(GPtrArray) all = g_ptr_array_new();
		guint i;

		for (i = 0; self->set != NULL && i < self->set->problems->len; i++)
			g_ptr_array_add(all, g_ptr_array_index(self->set->problems, i));
		for (i = 0; i < self->warnings->len; i++)
			g_ptr_array_add(all, g_ptr_array_index(self->warnings, i));
		g_ptr_array_add(all, NULL);
		return strv_reply((GStrv)all->pdata);
	}
	if (g_strcmp0(verb, "add") == 0)
		return cmd_add(self, args);
	if (g_strcmp0(verb, "remove") == 0)
		return *arg != '\0' ? cmd_remove(self, arg)
		                    : g_strdup("ERROR hotkey-remove needs an id");
	if (g_strcmp0(verb, "enable") == 0 || g_strcmp0(verb, "disable") == 0
	    || g_strcmp0(verb, "toggle") == 0) {
		t = verb[0] == 'e' ? HK_TOGGLE_ON : verb[0] == 'd' ? HK_TOGGLE_OFF
		                                                 : HK_TOGGLE_TOGGLE;
		if (!hk_set_hotkey_enabled(self, arg, t))
			return g_strdup_printf("ERROR no hotkey %s", arg);
		return g_strdup_printf("OK %s %s",
		                       hk_set_find_hotkey(self->set, arg)->enabled
		                       ? "enabled" : "disabled", arg);
	}
	if (g_strcmp0(verb, "suspend") == 0) {
		if (!parse_toggle_arg(arg, &t) || t > HK_TOGGLE_TOGGLE)
			return g_strdup("ERROR hotkey-suspend takes on, off or toggle");
		hk_set_suspended(self, t == HK_TOGGLE_TOGGLE
		                 ? !hk_engine_get_suspended(self->engine)
		                 : t == HK_TOGGLE_ON, "ipc");
		return g_strdup_printf("OK %s", hk_engine_get_suspended(self->engine)
		                       ? "suspended" : "on");
	}
	if (g_strcmp0(verb, "mode") == 0) {
		GError *e = NULL;

		if (*arg == '\0')
			return g_strdup_printf("OK %s", self->compositor != NULL
				? gowl_compositor_get_key_mode(self->compositor) : "default");
		if (!hk_enter_mode(self, arg, &e)) {
			gchar *r = g_strdup_printf("ERROR %s", e->message);

			g_error_free(e);
			return r;
		}
		return g_strdup_printf("OK %s", arg);
	}
	if (g_strcmp0(verb, "send") == 0 || g_strcmp0(verb, "send-text") == 0) {
		GError *e = NULL;

		/* The raw argument: leading and trailing spaces are keys too */
		if (args == NULL || *args == '\0')
			return g_strdup_printf("ERROR hotkey-%s needs keys", verb);
		if (!hk_send(self, args, verb[4] == '-', NULL, &e)) {
			gchar *r = g_strdup_printf("ERROR %s", e->message);

			g_error_free(e);
			return r;
		}
		return g_strdup("OK queued");
	}
	if (g_strcmp0(verb, "fire") == 0) {
		HkHotkey *hk = self->set != NULL ? hk_set_find_hotkey(self->set, arg)
		                                 : NULL;

		if (hk == NULL)
			return g_strdup_printf("ERROR no hotkey %s", arg);
		hk_queue_fire(self, hk);
		return g_strdup_printf("OK fired %s", arg);
	}
	if (g_strcmp0(verb, "call") == 0) {
		g_auto(GStrv) argv = NULL;
		GError *e = NULL;
		gchar *result;

		if (*arg == '\0' || !g_shell_parse_argv(arg, NULL, &argv, &e)) {
			gchar *r = g_strdup_printf("ERROR hotkey-call NAME [ARGS]%s%s",
			                           e != NULL ? ": " : "",
			                           e != NULL ? e->message : "");

			g_clear_error(&e);
			return r;
		}
		result = hk_call_function_now(self, argv[0],
		                              (const gchar * const *)argv + 1,
		                              "ipc", &e);
		if (result == NULL) {
			gchar *r = g_strdup_printf("ERROR %s", e->message);

			g_error_free(e);
			return r;
		}
		{
			gchar *r = g_strdup_printf("OK %s", result);

			g_free(result);
			return r;
		}
	}
	if (g_strcmp0(verb, "reload") == 0) {
		hk_reload(self);
		return g_strdup_printf("OK %u hotkey(s), %u problem(s)",
		                       self->set->hotkeys->len,
		                       self->set->problems->len);
	}
	if (g_strcmp0(verb, "history") == 0)
		return cmd_history(self);
	if (g_strcmp0(verb, "keystate") == 0)
		return *arg != '\0' ? cmd_keystate(self, arg)
		                    : g_strdup("ERROR hotkey-keystate needs a key");
	if (g_strcmp0(verb, "capslock") == 0 || g_strcmp0(verb, "numlock") == 0) {
		if (!parse_toggle_arg(arg, &t))
			return g_strdup_printf("ERROR hotkey-%s takes on, off, toggle, "
			                       "always-on, always-off or normal", verb);
		hk_set_lock(self, verb[0] == 'c', t);
		return g_strdup("OK");
	}
	if (g_strcmp0(verb, "context-set") == 0) {
		gchar *rest = NULL;
		gchar *name = split_word(arg, &rest);

		if (*name == '\0' || !parse_toggle_arg(rest, &t)
		    || t == HK_TOGGLE_TOGGLE || t > HK_TOGGLE_ON)
			return g_strdup("ERROR hotkey-context-set NAME 0|1");
		gowl_hotkey_set_context(name, t == HK_TOGGLE_ON);
		return g_strdup_printf("OK %s %d", name, t == HK_TOGGLE_ON);
	}
	if (g_strcmp0(verb, "context-clear") == 0) {
		gowl_hotkey_clear_context(arg);
		return g_strdup("OK");
	}
	if (g_strcmp0(verb, "contexts") == 0) {
		g_auto(GStrv) v = gowl_hotkey_list_contexts();
		g_autoptr(GPtrArray) all = g_ptr_array_new_with_free_func(g_free);
		GHashTableIter it;
		gpointer k;
		guint i;

		if (self->set != NULL) {
			g_hash_table_iter_init(&it, self->set->contexts);
			while (g_hash_table_iter_next(&it, &k, NULL))
				g_ptr_array_add(all, g_strdup(k));
		}
		for (i = 0; v != NULL && v[i] != NULL; i++)
			g_ptr_array_add(all, g_strdup(v[i]));
		g_ptr_array_add(all, NULL);
		return strv_reply((GStrv)all->pdata);
	}
	if (g_strcmp0(verb, "func-define") == 0) {
		gchar *rest = NULL;
		gchar *name = split_word(arg, &rest);

		if (*name == '\0' || *rest == '\0')
			return g_strdup("ERROR hotkey-func-define NAME FORM (FORM is the "
			                "custom action's argument; %i, %a and %t are "
			                "substituted)");
		g_hash_table_replace(self->elisp_funcs, g_strdup(name),
		                     g_strdup(rest));
		return g_strdup_printf("OK defined %s", name);
	}
	if (g_strcmp0(verb, "func-undefine") == 0)
		return g_strdup(g_hash_table_remove(self->elisp_funcs, arg)
		                ? "OK" : "ERROR no such function");
	if (g_strcmp0(verb, "functions") == 0) {
		g_auto(GStrv) v = gowl_hotkey_list_funcs();
		g_autoptr(GPtrArray) all = g_ptr_array_new_with_free_func(g_free);
		GHashTableIter it;
		gpointer k;
		guint i;

		if (self->set != NULL) {
			g_hash_table_iter_init(&it, self->set->functions);
			while (g_hash_table_iter_next(&it, &k, NULL))
				g_ptr_array_add(all, g_strdup(k));
		}
		for (i = 0; v != NULL && v[i] != NULL; i++)
			g_ptr_array_add(all, g_strdup(v[i]));
		g_hash_table_iter_init(&it, self->elisp_funcs);
		while (g_hash_table_iter_next(&it, &k, NULL))
			g_ptr_array_add(all, g_strdup(k));
		g_ptr_array_add(all, NULL);
		return strv_reply((GStrv)all->pdata);
	}
	if (g_strcmp0(verb, "input-start") == 0) {
		gchar *rest = NULL;
		gchar *name = split_word(arg, &rest);

		if (self->input.active)
			hk_input_stop(self, "replaced");
		hk_input_start(self, *name != '\0' ? name : "input", rest, NULL, NULL);
		return g_strdup("OK started");
	}
	if (g_strcmp0(verb, "input-stop") == 0) {
		if (!self->input.active)
			return g_strdup("ERROR no input hook is running");
		hk_input_stop(self, "stopped");
		return g_strdup("OK stopped");
	}
	if (g_strcmp0(verb, "timers") == 0)
		return cmd_timers(self);
	if (g_strcmp0(verb, "timer-enable") == 0
	    || g_strcmp0(verb, "timer-disable") == 0) {
		HkTimerRun *tr = find_timer(self, arg);

		if (tr == NULL)
			return g_strdup_printf("ERROR no timer %s", arg);
		tr->enabled = verb[6] == 'e';
		wl_event_source_timer_update(tr->source, tr->enabled
			? (gint)MAX(tr->def->period_ms, 1) : 0);
		return g_strdup_printf("OK %s %s", tr->enabled ? "enabled"
		                                               : "disabled", arg);
	}
	if (g_strcmp0(verb, "cancel") == 0) {
		guint n = self->runners->len;

		hk_cancel_all(self);
		return g_strdup_printf("OK cancelled %u", n);
	}
	return g_strdup_printf("ERROR unknown command hotkey-%s (hotkey-help "
	                       "lists them)", verb);
}

static gchar *
hk_handle_command(
	GowlIpcHandler *handler,
	const gchar    *command,
	const gchar    *args
){
	if (command == NULL || !g_str_has_prefix(command, HK_PREFIX))
		return NULL;
	return hk_dispatch(GOWL_MODULE_HOTKEY(handler),
	                   command + strlen(HK_PREFIX), args);
}

static void
hk_ipc_init(GowlIpcHandlerInterface *iface)
{
	iface->handle_command = hk_handle_command;
}

/* ── JSON-RPC methods ────────────────────────────────────────────────── */

/* An `OK ...'/`ERROR ...' reply as a method result: OK with JSON after
 * it is that JSON, OK with text is {"result": TEXT}, ERROR is an error. */
static JsonNode *
reply_to_result(
	gchar   *reply,
	GError **error
){
	g_autofree gchar *r = reply;
	const gchar *rest;
	g_autoptr(JsonParser) p = NULL;

	if (r == NULL || !g_str_has_prefix(r, "OK")) {
		g_set_error(error, GOWL_IPC_ERROR, GOWL_IPC_ERROR_FAILED, "%s",
		            r != NULL && g_str_has_prefix(r, "ERROR ") ? r + 6
		            : "no reply");
		return NULL;
	}
	rest = r + 2;
	while (*rest == ' ')
		rest++;
	if (*rest == '{' || *rest == '[') {
		p = json_parser_new();
		if (json_parser_load_from_data(p, rest, -1, NULL))
			return json_node_copy(json_parser_get_root(p));
	}
	{
		g_autoptr(JsonBuilder) b = json_builder_new();

		json_builder_begin_object(b);
		json_builder_set_member_name(b, "result");
		json_builder_add_string_value(b, rest);
		json_builder_end_object(b);
		return json_builder_get_root(b);
	}
}

static const gchar *
param_str(
	JsonObject  *params,
	const gchar *name
){
	JsonNode *n;

	if (!json_object_has_member(params, name))
		return NULL;
	n = json_object_get_member(params, name);
	if (n == NULL || !JSON_NODE_HOLDS_VALUE(n)
	    || json_node_get_value_type(n) != G_TYPE_STRING)
		return NULL;
	return json_node_get_string(n);
}

static JsonNode *
m_list(
	GowlIpc             *ipc,
	const GowlIpcCaller *caller,
	JsonObject          *params,
	gpointer             data,
	GError             **error
){
	(void)ipc;
	(void)caller;
	(void)params;
	return reply_to_result(hk_dispatch(data, "list", NULL), error);
}

static JsonNode *
m_status(
	GowlIpc             *ipc,
	const GowlIpcCaller *caller,
	JsonObject          *params,
	gpointer             data,
	GError             **error
){
	(void)ipc;
	(void)caller;
	(void)params;
	return reply_to_result(hk_dispatch(data, "status", NULL), error);
}

static JsonNode *
m_add(
	GowlIpc             *ipc,
	const GowlIpcCaller *caller,
	JsonObject          *params,
	gpointer             data,
	GError             **error
){
	(void)ipc;
	(void)caller;
	return reply_to_result(hk_dispatch(data, "add",
	                                   param_str(params, "yaml")), error);
}

static JsonNode *
m_remove(
	GowlIpc             *ipc,
	const GowlIpcCaller *caller,
	JsonObject          *params,
	gpointer             data,
	GError             **error
){
	(void)ipc;
	(void)caller;
	return reply_to_result(hk_dispatch(data, "remove",
	                                   param_str(params, "id")), error);
}

/* hotkey.set: {id, enabled} | {suspended} | {mode} -- each given one
 * applied in that order. */
static JsonNode *
m_set(
	GowlIpc             *ipc,
	const GowlIpcCaller *caller,
	JsonObject          *params,
	gpointer             data,
	GError             **error
){
	GowlModuleHotkey *self = data;
	gchar *r;

	(void)ipc;
	(void)caller;
	if (json_object_has_member(params, "enabled")) {
		JsonNode *n = json_object_get_member(params, "enabled");
		const gchar *id = param_str(params, "id");
		const gchar *how;

		if (id == NULL) {
			g_set_error(error, GOWL_IPC_ERROR, GOWL_IPC_ERROR_INVALID_PARAMS,
			            "enabled needs id");
			return NULL;
		}
		how = json_node_get_value_type(n) == G_TYPE_STRING ? "toggle"
		      : json_node_get_boolean(n) ? "enable" : "disable";
		r = hk_dispatch(self, how, id);
		if (!g_str_has_prefix(r, "OK"))
			return reply_to_result(r, error);
		g_free(r);
	}
	if (json_object_has_member(params, "suspended")) {
		JsonNode *n = json_object_get_member(params, "suspended");
		const gchar *how = json_node_get_value_type(n) == G_TYPE_STRING
		                   ? "toggle" : json_node_get_boolean(n) ? "on" : "off";

		g_free(hk_dispatch(self, "suspend", how));
	}
	if (param_str(params, "mode") != NULL) {
		r = hk_dispatch(self, "mode", param_str(params, "mode"));
		if (!g_str_has_prefix(r, "OK"))
			return reply_to_result(r, error);
		g_free(r);
	}
	return reply_to_result(hk_dispatch(self, "status", NULL), error);
}

static JsonNode *
m_send(
	GowlIpc             *ipc,
	const GowlIpcCaller *caller,
	JsonObject          *params,
	gpointer             data,
	GError             **error
){
	(void)ipc;
	(void)caller;
	if (param_str(params, "text") != NULL)
		return reply_to_result(hk_dispatch(data, "send-text",
		                                   param_str(params, "text")), error);
	return reply_to_result(hk_dispatch(data, "send",
	                                   param_str(params, "keys")), error);
}

static JsonNode *
m_fire(
	GowlIpc             *ipc,
	const GowlIpcCaller *caller,
	JsonObject          *params,
	gpointer             data,
	GError             **error
){
	(void)ipc;
	(void)caller;
	return reply_to_result(hk_dispatch(data, "fire", param_str(params, "id")),
	                       error);
}

static JsonNode *
m_call(
	GowlIpc             *ipc,
	const GowlIpcCaller *caller,
	JsonObject          *params,
	gpointer             data,
	GError             **error
){
	g_autoptr(GString) line = g_string_new(NULL);
	g_autofree gchar *qname = NULL;
	const gchar *name = param_str(params, "name");
	guint i;

	(void)ipc;
	(void)caller;
	qname = g_shell_quote(name != NULL ? name : "");
	g_string_append(line, qname);
	if (json_object_has_member(params, "args")) {
		JsonArray *a = json_object_get_array_member(params, "args");

		for (i = 0; a != NULL && i < json_array_get_length(a); i++) {
			JsonNode *n = json_array_get_element(a, i);
			g_autofree gchar *q = NULL;

			if (!JSON_NODE_HOLDS_VALUE(n)
			    || json_node_get_value_type(n) != G_TYPE_STRING) {
				g_set_error(error, GOWL_IPC_ERROR,
				            GOWL_IPC_ERROR_INVALID_PARAMS,
				            "args are strings");
				return NULL;
			}
			q = g_shell_quote(json_node_get_string(n));
			g_string_append_printf(line, " %s", q);
		}
	}
	return reply_to_result(hk_dispatch(data, "call", line->str), error);
}

static const GowlIpcParam p_none[] = { { NULL, 0, FALSE, NULL } };
static const GowlIpcParam p_add[] = {
	{ "yaml", GOWL_IPC_PARAM_STRING, TRUE,
	  "a hotkey entry ({key: \"#t\", do: ...}) or a hotkey: section" },
	{ NULL, 0, FALSE, NULL }
};
static const GowlIpcParam p_id[] = {
	{ "id", GOWL_IPC_PARAM_STRING, TRUE, "a hotkey's id (hotkey.list)" },
	{ NULL, 0, FALSE, NULL }
};
static const GowlIpcParam p_set[] = {
	{ "id", GOWL_IPC_PARAM_STRING, FALSE, "the hotkey `enabled' is about" },
	{ "enabled", GOWL_IPC_PARAM_TOGGLE, FALSE, "true, false or \"toggle\"" },
	{ "suspended", GOWL_IPC_PARAM_TOGGLE, FALSE, "true, false or \"toggle\"" },
	{ "mode", GOWL_IPC_PARAM_STRING, FALSE, "a mode to enter; \"default\" leaves" },
	{ NULL, 0, FALSE, NULL }
};
static const GowlIpcParam p_send[] = {
	{ "keys", GOWL_IPC_PARAM_STRING, FALSE,
	  "AutoHotkey Send syntax: \"^c\", \"{Enter}\", \"hello{Tab}\"" },
	{ "text", GOWL_IPC_PARAM_STRING, FALSE, "literal text instead" },
	{ NULL, 0, FALSE, NULL }
};
static const GowlIpcParam p_call[] = {
	{ "name", GOWL_IPC_PARAM_STRING, TRUE, "a function (hotkey-functions)" },
	{ "args", GOWL_IPC_PARAM_ARRAY, FALSE, "its arguments, strings" },
	{ NULL, 0, FALSE, NULL }
};

static const struct {
	const gchar        *name;
	const gchar        *summary;
	const GowlIpcParam *params;
	const gchar        *result;
	GowlIpcMethodFunc   func;
} methods[] = {
	{ "hotkey.list", "Every hotkey and remap, with where it came from",
	  p_none, "[{id, key, canonical, kind, enabled, mode, actions, ...}]",
	  m_list },
	{ "hotkey.status", "The module's state: suspended, mode, counts",
	  p_none, "{active, suspended, mode, hotkeys, ...}", m_status },
	{ "hotkey.add", "Add a hotkey (or a section) for this session",
	  p_add, "{result}", m_add },
	{ "hotkey.remove", "Remove a hotkey added at run time (else disable it)",
	  p_id, "{result}", m_remove },
	{ "hotkey.set", "Enable or disable a hotkey, suspend, enter a mode",
	  p_set, "the status", m_set },
	{ "hotkey.send", "Type keys (Send syntax) or text into the focused window",
	  p_send, "{result}", m_send },
	{ "hotkey.fire", "Run a hotkey's actions as if it were pressed",
	  p_id, "{result}", m_fire },
	{ "hotkey.call", "Call a function and return what it gave",
	  p_call, "{result}", m_call },
	{ NULL, NULL, NULL, NULL, NULL }
};

static void
methods_install(GowlModuleHotkey *self)
{
	GowlIpc *ipc;
	guint i;

	if (self->compositor == NULL)
		return;
	ipc = gowl_compositor_get_ipc(self->compositor);
	if (ipc == NULL || ipc == self->ipc)
		return;
	for (i = 0; methods[i].name != NULL; i++)
		gowl_ipc_register_method(ipc, methods[i].name, methods[i].summary,
		                         methods[i].params, methods[i].result,
		                         methods[i].func, self, NULL);
	gowl_ipc_declare_event(ipc, "hotkey",
	                       "the hotkey module: fired ID, mode NAME, "
	                       "suspended on|off, input NAME REASON TEXT, fault "
	                       "TEXT");
	self->ipc = ipc;
	g_object_add_weak_pointer(G_OBJECT(ipc), (gpointer *)&self->ipc);
}

static void
methods_uninstall(GowlModuleHotkey *self)
{
	guint i;

	if (self->ipc == NULL)
		return;
	for (i = 0; methods[i].name != NULL; i++)
		gowl_ipc_unregister_method(self->ipc, methods[i].name);
	g_object_remove_weak_pointer(G_OBJECT(self->ipc), (gpointer *)&self->ipc);
	self->ipc = NULL;
}

/* The compositor was given another IPC server (cmacs starts it from
 * Lisp, after the modules): the methods move to it. */
static void
on_ipc_changed(
	GowlCompositor *compositor,
	GowlIpc        *ipc,
	gpointer        data
){
	GowlModuleHotkey *self = GOWL_MODULE_HOTKEY(data);

	(void)compositor;
	(void)ipc;
	methods_uninstall(self);
	if (self->active)
		methods_install(self);
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

static void
set_suspend_key(
	GowlModuleHotkey *self,
	const gchar      *value
){
	guint m = 0;
	guint s = 0;

	if (g_ascii_strcasecmp(value, "none") == 0 || *value == '\0') {
		g_free(self->suspend_text);
		self->suspend_text = g_strdup("none");
		self->suspend_mods = 0;
		self->suspend_sym = 0;
		return;
	}
	if (!gowl_keybind_parse(value, &m, &s) || s == 0) {
		g_warning("hotkey: suspend-key '%s' is no keybind; keeping %s",
		          value, self->suspend_text);
		return;
	}
	g_free(self->suspend_text);
	self->suspend_text = g_strdup(value);
	self->suspend_mods = m;
	self->suspend_sym = s;
}

/*
 * hk_configure:
 * @mod: the module
 * @config: a #GHashTable of string settings from `modules: hotkey:'
 *
 * suspend-key (a keybind or "none"), key-delay (ms between sent keys),
 * send-mode (input|event), send-level (0..100), accept-synthetic,
 * toast, log, hotstrings, dbus (booleans), c-timeout (ms), scripts
 * (crispy hotkey scripts, a list).  A bad value is refused with a
 * warning and the old one kept.  Hotkeys themselves live in the
 * top-level `hotkey:' section.
 */
static void
hk_configure(
	GowlModule *mod,
	gpointer    config
){
	GowlModuleHotkey *self = GOWL_MODULE_HOTKEY(mod);
	GHashTable *settings = (GHashTable *)config;
	const gchar *value;
	gboolean b;
	guint64 n;

	if (settings == NULL)
		return;
	if ((value = g_hash_table_lookup(settings, "suspend-key")) != NULL)
		set_suspend_key(self, value);
	if ((value = g_hash_table_lookup(settings, "key-delay")) != NULL) {
		if (g_ascii_string_to_unsigned(value, 10, 0, 1000, &n, NULL))
			self->key_delay_ms = (guint)n;
		else
			g_warning("hotkey: key-delay '%s' -- 0..1000", value);
	}
	if ((value = g_hash_table_lookup(settings, "send-mode")) != NULL) {
		if (g_ascii_strcasecmp(value, "event") == 0)
			self->event_mode = TRUE;
		else if (g_ascii_strcasecmp(value, "input") == 0)
			self->event_mode = FALSE;
		else
			g_warning("hotkey: send-mode '%s' -- input or event", value);
	}
	if ((value = g_hash_table_lookup(settings, "send-level")) != NULL) {
		if (g_ascii_string_to_unsigned(value, 10, 0, HK_MAX_LEVEL, &n, NULL))
			self->send_level = (gint)n;
		else
			g_warning("hotkey: send-level '%s' -- 0..%d", value, HK_MAX_LEVEL);
	}
	if ((value = g_hash_table_lookup(settings, "c-timeout")) != NULL) {
		if (g_ascii_string_to_unsigned(value, 10, 10, 60000, &n, NULL))
			self->c_timeout_ms = (guint)n;
		else
			g_warning("hotkey: c-timeout '%s' -- 10..60000", value);
	}
#define BOOL_SETTING(key, field) \
	if ((value = g_hash_table_lookup(settings, key)) != NULL) { \
		if (parse_bool_setting(value, &b)) \
			self->field = b; \
		else \
			g_warning("hotkey: " key " '%s' is no yes/no", value); \
	}
	BOOL_SETTING("accept-synthetic", accept_synthetic)
	BOOL_SETTING("toast", toast)
	BOOL_SETTING("log", log)
	BOOL_SETTING("hotstrings", hotstrings)
	BOOL_SETTING("dbus", dbus)
#undef BOOL_SETTING
	hk_engine_set_accept_synthetic(self->engine, self->accept_synthetic);
	/* dbus switched after startup: the service follows */
	if (self->started && self->active) {
		if (self->dbus && self->dbus_service == NULL)
			self->dbus_service = hk_dbus_new(self);
		else if (!self->dbus && self->dbus_service != NULL) {
			hk_dbus_free(self->dbus_service);
			self->dbus_service = NULL;
		}
	}
	if ((value = g_hash_table_lookup(settings, "scripts")) != NULL
	    || (value = g_hash_table_lookup(settings, "script")) != NULL) {
		g_strfreev(self->setting_scripts);
		self->setting_scripts = g_strsplit(value, "\n", -1);
		if (self->started)
			hk_scripts_sync(self);
	}
	if (self->started)
		install_hotstrings(self);
}

static gboolean
hk_activate(GowlModule *mod)
{
	GowlModuleHotkey *self = GOWL_MODULE_HOTKEY(mod);

	self->active = TRUE;
	if (self->macro_listener == 0)
		hk_runner_init(self);
	/* Enabled after startup (cmacs's gowl-enable-module): the compositor
	 * is already known.  At first start the startup handler does this. */
	if (self->compositor != NULL) {
		sync_config(self);
		methods_install(self);
	}
	return TRUE;
}

static void
hk_deactivate(GowlModule *mod)
{
	GowlModuleHotkey *self = GOWL_MODULE_HOTKEY(mod);
	g_autoptr(GArray) out = hk_out_new();

	if (self->compositor != NULL) {
		hk_engine_reset(self->engine, out);
		hk_play_outputs(self, out);
		hk_out_clear(out);
	}
	hk_overlay_detach(self);
	hk_cancel_all(self);
	if (self->input.active)
		hk_input_stop(self, "deactivated");
	g_ptr_array_set_size(self->timers, 0);
	if (self->hotstrings_installed)
		gowl_gabbrev_remove_source("hotkey");
	self->hotstrings_installed = FALSE;
	methods_uninstall(self);
	if (self->dbus_service != NULL) {
		hk_dbus_free(self->dbus_service);
		self->dbus_service = NULL;
	}
	hk_scripts_unload(self);
	hk_runner_finish(self);
	self->active = FALSE;
}

static const gchar *
hk_get_name(GowlModule *mod)
{
	(void)mod;
	return "hotkey";
}

static const gchar *
hk_get_description(GowlModule *mod)
{
	(void)mod;
	return "AutoHotkey for gowl: hotkeys, combinations, remaps, modes, "
	       "contexts, timers and hooks running gowl, macro, shell, C and "
	       "Elisp actions";
}

static const gchar *
hk_get_version(GowlModule *mod)
{
	(void)mod;
	return "0.1.0";
}

/* ── Startup / shutdown ──────────────────────────────────────────────── */

/* Event sources belong to the display's event loop. */
static void
hk_display_destroyed(
	struct wl_listener *listener,
	void               *data
){
	GowlModuleHotkey *self;

	(void)data;
	self = wl_container_of(listener, self, display_destroy);
	hk_overlay_detach(self);
	hk_cancel_all(self);
	g_ptr_array_set_size(self->timers, 0);
	if (self->deadline_timer != NULL) {
		wl_event_source_remove(self->deadline_timer);
		self->deadline_timer = NULL;
	}
	if (self->restore_idle != NULL) {
		wl_event_source_remove(self->restore_idle);
		self->restore_idle = NULL;
	}
	hk_runner_finish(self);
	hk_post_close(self);
	wl_list_remove(&self->display_destroy.link);
	wl_list_init(&self->display_destroy.link);
}

static void
hk_on_startup(
	GowlStartupHandler *handler,
	gpointer            compositor
){
	GowlModuleHotkey *self = GOWL_MODULE_HOTKEY(handler);
	struct wl_display *display;
	GList *l;

	if (self->compositor == NULL) {
		self->compositor = GOWL_COMPOSITOR(compositor);
		g_object_add_weak_pointer(G_OBJECT(compositor),
		                          (gpointer *)&self->compositor);
		g_object_weak_ref(G_OBJECT(compositor), on_compositor_gone, self);
		g_signal_connect_object(compositor, "focus-changed",
		                        G_CALLBACK(on_focus_changed), self, 0);
		g_signal_connect_object(compositor, "client-added",
		                        G_CALLBACK(on_client_added), self, 0);
		g_signal_connect_object(compositor, "client-removed",
		                        G_CALLBACK(on_client_removed), self, 0);
		g_signal_connect_object(compositor, "client-title-changed",
		                        G_CALLBACK(on_title_changed), self, 0);
		g_signal_connect_object(compositor, "lock-changed",
		                        G_CALLBACK(on_lock_changed), self, 0);
		g_signal_connect_object(compositor, "mode-changed",
		                        G_CALLBACK(on_mode_changed), self, 0);
		g_signal_connect_object(compositor, "monitor-added",
		                        G_CALLBACK(on_monitor_added), self, 0);
		g_signal_connect_object(compositor, "arranged",
		                        G_CALLBACK(on_arranged), self, 0);
		g_signal_connect_object(compositor, "ipc-changed",
		                        G_CALLBACK(on_ipc_changed), self, 0);
	}
	display = gowl_compositor_get_wl_display(self->compositor);
	if (display != NULL && wl_list_empty(&self->display_destroy.link))
		wl_display_add_destroy_listener(display, &self->display_destroy);
	/* The tags each monitor shows now, so the first switch is a switch */
	for (l = gowl_compositor_get_monitors(self->compositor); l != NULL;
	     l = l->next)
		g_object_set_data(G_OBJECT(l->data), "gowl-hotkey-tags",
			GUINT_TO_POINTER(gowl_monitor_get_tags(GOWL_MONITOR(l->data))));

	if (self->macro_listener == 0)
		hk_runner_init(self);
	hk_post_open(self);
	self->started = TRUE;
	sync_config(self);
	if (self->set == NULL)
		hk_reload(self);
	timers_restart(self);
	hk_scripts_sync(self);
	methods_install(self);
	if (self->dbus && self->dbus_service == NULL && self->active)
		self->dbus_service = hk_dbus_new(self);
	if (self->active)
		run_hooks(self, "startup", NULL, NULL);
	hk_log(self, "started; %s suspends", self->suspend_text);
}

static void
hk_startup_init(GowlStartupHandlerInterface *iface)
{
	iface->on_startup = hk_on_startup;
}

/* Shutdown: let go of every remap target while the compositor can
 * still send, and stop every child. */
static void
hk_on_shutdown(
	GowlShutdownHandler *handler,
	gpointer             compositor
){
	GowlModuleHotkey *self = GOWL_MODULE_HOTKEY(handler);
	g_autoptr(GArray) out = hk_out_new();

	(void)compositor;
	hk_engine_reset(self->engine, out);
	hk_play_outputs(self, out);
	hk_out_clear(out);
	hk_overlay_detach(self);
	hk_cancel_all(self);
	g_ptr_array_set_size(self->timers, 0);
	methods_uninstall(self);
	if (self->dbus_service != NULL) {
		hk_dbus_free(self->dbus_service);
		self->dbus_service = NULL;
	}
	hk_scripts_unload(self);
	hk_runner_finish(self);
	hk_post_close(self);
}

static void
hk_shutdown_init(GowlShutdownHandlerInterface *iface)
{
	iface->on_shutdown = hk_on_shutdown;
}

/* ── GObject lifecycle ───────────────────────────────────────────────── */

static void
gowl_module_hotkey_finalize(GObject *object)
{
	GowlModuleHotkey *self = GOWL_MODULE_HOTKEY(object);

	wl_list_remove(&self->display_destroy.link);
	hk_cancel_all(self);
	g_ptr_array_unref(self->timers);
	if (self->deadline_timer != NULL)
		wl_event_source_remove(self->deadline_timer);
	if (self->restore_idle != NULL)
		wl_event_source_remove(self->restore_idle);
	methods_uninstall(self);
	if (self->dbus_service != NULL)
		hk_dbus_free(self->dbus_service);
	hk_runner_finish(self);
	hk_post_close(self);
	if (self->config != NULL && self->reloaded_handler != 0)
		g_signal_handler_disconnect(self->config, self->reloaded_handler);
	g_clear_object(&self->config);
	if (self->compositor != NULL) {
		g_object_weak_unref(G_OBJECT(self->compositor), on_compositor_gone,
		                    self);
		hk_overlay_detach(self);
		g_object_remove_weak_pointer(G_OBJECT(self->compositor),
		                             (gpointer *)&self->compositor);
	}
	if (self->hotstrings_installed)
		gowl_gabbrev_remove_source("hotkey");
	if (self->set != NULL)
		hk_set_unref(self->set);
	hk_engine_free(self->engine);
	g_ptr_array_unref(self->runtime);
	g_hash_table_unref(self->enabled);
	g_ptr_array_unref(self->warnings);
	g_hash_table_unref(self->swallowed);
	g_clear_pointer(&self->ev_fields, g_hash_table_unref);
	g_free(self->suspend_text);
	g_strfreev(self->setting_scripts);
	g_free(self->last_error);
	g_free(self->mode);
	g_queue_free(self->fires);
	g_ptr_array_unref(self->runners);
	g_hash_table_unref(self->running);
	g_queue_free(self->plays);
	g_hash_table_unref(self->elisp_funcs);
	g_hash_table_unref(self->c_funcs);
	g_hash_table_unref(self->c_building);

	g_hash_table_unref(self->c_faulted);
	g_hash_table_unref(self->scripts);
	g_free(self->cache_dir);
	g_queue_free(self->posted);
	g_mutex_clear(&self->post_lock);
	g_hash_table_unref(self->watch_dirs);
	if (self->input.text != NULL)
		g_string_free(self->input.text, TRUE);
	if (self->input.end_keys != NULL)
		g_ptr_array_unref(self->input.end_keys);
	g_strfreev(self->input.match);
	g_free(self->input.name);
	if (self->input.swallowed != NULL)
		g_hash_table_unref(self->input.swallowed);

	G_OBJECT_CLASS(gowl_module_hotkey_parent_class)->finalize(object);
}

static void
gowl_module_hotkey_class_init(GowlModuleHotkeyClass *klass)
{
	GObjectClass    *object_class = G_OBJECT_CLASS(klass);
	GowlModuleClass *module_class = GOWL_MODULE_CLASS(klass);

	object_class->finalize = gowl_module_hotkey_finalize;

	module_class->activate        = hk_activate;
	module_class->deactivate      = hk_deactivate;
	module_class->get_name        = hk_get_name;
	module_class->get_description = hk_get_description;
	module_class->get_version     = hk_get_version;
	module_class->configure       = hk_configure;
}

static void
gowl_module_hotkey_init(GowlModuleHotkey *self)
{
	const gchar *cache;

	wl_list_init(&self->display_destroy.link);
	self->display_destroy.notify = hk_display_destroyed;
	wl_list_init(&self->group_mods.link);
	self->engine = hk_engine_new();
	hk_engine_set_allow_func(self->engine, allow_hotkey, self);
	self->runtime = g_ptr_array_new_with_free_func(
		(GDestroyNotify)gowl_hotkey_source_free);
	self->enabled = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
	                                      NULL);
	self->warnings = g_ptr_array_new_with_free_func(g_free);
	self->suspend_text = g_strdup("none");
	set_suspend_key(self, HK_DEFAULT_SUSPEND_KEY);
	self->toast = TRUE;
	self->hotstrings = TRUE;
	self->dbus = FALSE;
	self->c_timeout_ms = HK_DEFAULT_C_TIMEOUT_MS;
	self->caps_force = HK_TOGGLE_NORMAL;
	self->num_force = HK_TOGGLE_NORMAL;
	self->restore_caps = -1;
	self->restore_num = -1;
	self->swallowed = g_hash_table_new(g_direct_hash, g_direct_equal);
	self->fires = g_queue_new();
	self->runners = g_ptr_array_new();
	self->running = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
	                                      NULL);
	self->plays = g_queue_new();
	self->elisp_funcs = g_hash_table_new_full(g_str_hash, g_str_equal,
	                                          g_free, g_free);
	self->timers = g_ptr_array_new_with_free_func(timer_run_free);
	self->c_funcs = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
	                                      NULL);
	self->c_building = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
	                                         NULL);

	self->c_faulted = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
	                                        g_free);
	self->scripts = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
	                                      NULL);
	cache = g_getenv("XDG_CACHE_HOME");
	self->cache_dir = cache != NULL && *cache != '\0'
	                  ? g_build_filename(cache, "gowl", "hotkey", NULL)
	                  : g_build_filename(g_get_home_dir(), ".cache", "gowl",
	                                     "hotkey", NULL);
	g_mutex_init(&self->post_lock);
	self->post_fd = -1;
	self->posted = g_queue_new();
	self->watch_fd = -1;
	self->watch_dirs = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
	                                         NULL);
}

/* ── Shared-object entry point ───────────────────────────────────────── */

G_MODULE_EXPORT GType
gowl_module_register(void)
{
	return GOWL_TYPE_MODULE_HOTKEY;
}
