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
#define G_LOG_DOMAIN "gowl-dropdown"

#include <glib-object.h>
#include <gmodule.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>
#include <wayland-server-core.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#include "module/gowl-module.h"
#include "interfaces/gowl-startup-handler.h"
#include "interfaces/gowl-keybind-handler.h"
#include "interfaces/gowl-dropdown-provider.h"
#include "config/gowl-config.h"
#include "config/gowl-keybind.h"
#include "core/gowl-compositor.h"
#include "core/gowl-client.h"
#include "core/gowl-monitor.h"
#include "gowl-enums.h"

/**
 * GowlModuleDropdown:
 *
 * Guake-style dropdown window module.  Reads a list of named
 * dropdown entries from the gowl config `dropdowns:` section.
 * Each entry is lazy-spawned on first keybind press, captured
 * via the compositor's prefloat-hint mechanism, and thereafter
 * toggled between a visible overlay position and a hidden stash
 * through the compositor overlay API and optional scene effects.
 *
 * Dropdowns always anchor to the currently focused output at
 * toggle-on, so they follow the user's focus across monitors.
 *
 * Keybinds come from the entry's own `keybind` field (per-entry);
 * the module implements GowlKeybindHandler to consume matching
 * key events and call @dropdown_entry_toggle().
 */

#define GOWL_TYPE_MODULE_DROPDOWN (gowl_module_dropdown_get_type())
G_DECLARE_FINAL_TYPE(GowlModuleDropdown, gowl_module_dropdown,
                     GOWL, MODULE_DROPDOWN, GowlModule)

/**
 * DropdownState:
 * @name: entry name (copied)
 * @spawn_cmd: command to spawn (copied)
 * @modifiers: parsed keybind modifier bitmask
 * @keysym: parsed keybind XKB keysym
 * @width_pct: fractional width (if absolute is 0)
 * @height_pct: fractional height (if absolute is 0)
 * @width_abs: absolute width in pixels (takes precedence)
 * @height_abs: absolute height in pixels (takes precedence)
 * @anchor: 0=top, 1=bottom, 2=left, 3=right
 * @visible: %TRUE if currently shown
 * @pid: process ID of the spawned child (0 = not spawned)
 * @client: captured #GowlClient after first map
 * @destroy_handler_id: connected destroy signal handler id on @client
 * @module: back-reference to the module
 *
 * One runtime dropdown instance built from a config entry.
 */
typedef struct {
	gchar    *name;
	gchar    *spawn_cmd;
	guint     modifiers;
	guint     keysym;
	gdouble   width_pct;
	gdouble   height_pct;
	gint      width_abs;
	gint      height_abs;
	gint      anchor;

	gboolean  visible;
	GPid      pid;
	struct wl_event_source *child_watch;
	GowlClient *client;
	gulong    destroy_handler_id;

	gpointer  module; /* GowlModuleDropdown * */
} DropdownState;

struct _GowlModuleDropdown {
	GowlModule      parent_instance;
	GowlCompositor *compositor;
	gulong focus_handler;
	struct wl_listener display_destroy;
	GPtrArray      *entries; /* DropdownState* owned */
};

static void dd_startup_init(GowlStartupHandlerInterface *iface);
static void dd_keybind_init(GowlKeybindHandlerInterface *iface);
static void dd_provider_init(GowlDropdownProviderInterface *iface);

G_DEFINE_TYPE_WITH_CODE(GowlModuleDropdown, gowl_module_dropdown,
	GOWL_TYPE_MODULE,
	G_IMPLEMENT_INTERFACE(GOWL_TYPE_STARTUP_HANDLER,
		dd_startup_init)
	G_IMPLEMENT_INTERFACE(GOWL_TYPE_KEYBIND_HANDLER,
		dd_keybind_init)
	G_IMPLEMENT_INTERFACE(GOWL_TYPE_DROPDOWN_PROVIDER,
		dd_provider_init))

/* Forward declarations */
static void dd_entry_toggle(GowlModuleDropdown *self, DropdownState *s);
static void dd_on_client_mapped(GowlCompositor *compositor,
                                 GowlClient     *c,
                                 gpointer        user_data);
static void dd_on_client_destroy(GowlClient *c, gpointer user_data);
static void dd_state_free(gpointer data);
static void dd_present(GowlModuleDropdown *self, DropdownState *s, gboolean visible);

/**
 * dd_compute_geometry:
 * @s: the state
 * @mon: the focused monitor
 * @out: (out): computed target rectangle
 *
 * Computes the target (x, y, width, height) rectangle for the
 * dropdown on its current monitor, honoring the entry's anchor
 * edge and width/height spec.
 */
static void
dd_compute_geometry(
	DropdownState *s,
	GowlMonitor   *mon,
	gint          *out_x,
	gint          *out_y,
	gint          *out_w,
	gint          *out_h
){
	gint mx, my, mw, mh;
	gint w, h;

	gowl_monitor_get_window_area(mon, &mx, &my, &mw, &mh);

	w = s->width_abs > 0
		? s->width_abs
		: (gint)(mw * (s->width_pct > 0 ? s->width_pct : 1.0));
	h = s->height_abs > 0
		? s->height_abs
		: (gint)(mh * (s->height_pct > 0 ? s->height_pct : (2.0 / 3.0)));

	w = CLAMP(w, 1, MAX(1, mw));
	h = CLAMP(h, 1, MAX(1, mh));

	switch (s->anchor) {
	case 1: /* bottom */
		*out_x = mx + (mw - w) / 2;
		*out_y = my + mh - h;
		break;
	case 2: /* left */
		*out_x = mx;
		*out_y = my + (mh - h) / 2;
		break;
	case 3: /* right */
		*out_x = mx + mw - w;
		*out_y = my + (mh - h) / 2;
		break;
	default: /* top */
		*out_x = mx + (mw - w) / 2;
		*out_y = my;
		break;
	}
	*out_w = w;
	*out_h = h;
}

/**
 * dd_state_free:
 * @data: a #DropdownState pointer
 *
 * Free function for DropdownState entries in the GPtrArray.
 * Disconnects the destroy handler if the client is still alive,
 * then frees all strings.
 */
static void
dd_state_free(gpointer data)
{
	DropdownState *s;

	s = (DropdownState *)data;
	if (s == NULL)
		return;

	if (s->client != NULL && s->destroy_handler_id != 0) {
		g_signal_handler_disconnect(s->client, s->destroy_handler_id);
		s->destroy_handler_id = 0;
	}
	if (s->child_watch != NULL)
		wl_event_source_remove(s->child_watch);
	if (s->module != NULL && ((GowlModuleDropdown *)s->module)->compositor != NULL)
		gowl_compositor_cancel_prefloat_hint(((GowlModuleDropdown *)s->module)->compositor, s->pid);
	g_free(s->name);
	g_free(s->spawn_cmd);
	g_free(s);
}

/**
 * dd_find_by_name:
 * @self: the module
 * @name: entry name to locate
 *
 * Returns: the matching state, or %NULL if no entry has that name
 */
static DropdownState *
dd_find_by_name(GowlModuleDropdown *self, const gchar *name)
{
	guint i;

	if (name == NULL || self->entries == NULL)
		return NULL;
	for (i = 0; i < self->entries->len; i++) {
		DropdownState *s;
		s = (DropdownState *)g_ptr_array_index(self->entries, i);
		if (g_strcmp0(s->name, name) == 0)
			return s;
	}
	return NULL;
}

/**
 * dd_find_by_key:
 * @self: the module
 * @modifiers: modifier bitmask
 * @keysym: XKB keysym
 *
 * Returns: the matching state, or %NULL if no entry has that key
 */
static DropdownState *
dd_find_by_key(
	GowlModuleDropdown *self,
	guint               modifiers,
	guint               keysym
){
	guint i;

	if (self->entries == NULL)
		return NULL;
	for (i = 0; i < self->entries->len; i++) {
		DropdownState *s;
		s = (DropdownState *)g_ptr_array_index(self->entries, i);
		if (s->keysym != 0 &&
		    s->modifiers == modifiers &&
		    s->keysym == keysym)
			return s;
		if (s->keysym == XKB_KEY_grave
		    && (keysym == XKB_KEY_asciitilde || keysym == XKB_KEY_grave)
		    && modifiers == (s->modifiers | GOWL_KEY_MOD_SHIFT))
			return s;
	}
	return NULL;
}

/**
 * dd_on_client_mapped:
 * @compositor: the compositor
 * @c: the mapped client
 * @user_data: the DropdownState * we registered with the hint
 *
 * Callback fired by the compositor after it matches a prefloat
 * hint's pid.  Stores the client pointer, connects a destroy
 * listener, and marks the entry visible.
 */
static void
dd_on_client_mapped(
	GowlCompositor *compositor,
	GowlClient     *c,
	gpointer        user_data
){
	DropdownState *s;

	(void)compositor;
	s = (DropdownState *)user_data;
	s->client = c;

	s->destroy_handler_id = g_signal_connect(
		c, "destroy",
		G_CALLBACK(dd_on_client_destroy), s);

	dd_present((GowlModuleDropdown *)s->module, s, s->visible);
	g_debug("dropdown: captured client for '%s'", s->name);
}

/**
 * dd_on_client_destroy:
 * @c: the client that was destroyed
 * @user_data: the DropdownState *
 *
 * Clears the entry's client/pid when the underlying process
 * exits so the next toggle re-spawns a fresh instance.
 */
static void
dd_on_client_destroy(
	GowlClient *c,
	gpointer    user_data
){
	DropdownState *s;

	(void)c;
	s = (DropdownState *)user_data;
	s->client = NULL;
	s->visible = FALSE;
	s->destroy_handler_id = 0;
	g_debug("dropdown: '%s' client destroyed, ready to re-spawn", s->name);
}

static void
dd_present(GowlModuleDropdown *self, DropdownState *s, gboolean visible)
{
	GowlMonitor *mon;
	gint x = 0, y = 0, w = 0, h = 0;

	s->visible = visible;
	if (s->client == NULL || self->compositor == NULL)
		return;
	mon = gowl_compositor_get_selected_monitor(self->compositor);
	if (visible) {
		if (mon == NULL) {
			s->visible = FALSE;
			return;
		}
		dd_compute_geometry(s, mon, &x, &y, &w, &h);
	}
	gowl_compositor_present_overlay(self->compositor, s->client, mon,
		x, y, w, h, s->anchor, visible);
}

/* Use the compositor loop: standalone Gowl does not run a GLib loop.
 * ECHILD also covers an embedding application's SIGCHLD handler reaping it. */
static int
dd_child_exited(void *data)
{
	DropdownState *s = data;
	GowlModuleDropdown *self = s->module;
	gint status;
	pid_t result = waitpid(s->pid, &status, WNOHANG);
	if (result == 0 || (result < 0 && errno != ECHILD)) {
		wl_event_source_timer_update(s->child_watch, 50);
		return 0;
	}
	wl_event_source_remove(s->child_watch);
	s->child_watch = NULL;
	if (self->compositor != NULL)
		gowl_compositor_cancel_prefloat_hint(self->compositor, s->pid);
	g_spawn_close_pid(s->pid);
	s->pid = 0;
	if (s->client == NULL)
		s->visible = FALSE;
	return 0;
}

static void
dd_focus_changed(GowlCompositor *compositor, GObject *focused, gpointer data)
{
	GowlModuleDropdown *self = data;
	guint i;
	for (i = 0; i < self->entries->len; i++) {
		DropdownState *s = g_ptr_array_index(self->entries, i);
		if (s->visible && s->client != NULL && G_OBJECT(s->client) != focused)
			dd_present(self, s, FALSE);
	}
}

/**
 * dd_entry_toggle:
 * @self: the module
 * @s: the entry state to toggle
 *
 * First press: spawns the entry's command, registers a prefloat
 * hint so the new client is captured and placed at the computed
 * geometry on the currently focused output.
 *
 * Subsequent presses: toggles fixed overlay visibility, re-computing the geometry for the
 * currently focused output each time it becomes visible so that
 * the dropdown follows focus across monitors.
 */
static void
dd_entry_toggle(GowlModuleDropdown *self, DropdownState *s)
{
	GowlMonitor *mon;
	gint x, y, w, h;

	if (self->compositor == NULL)
		return;

	mon = gowl_compositor_get_selected_monitor(self->compositor);

	if (mon == NULL) {
		g_warning("dropdown: no monitor to place '%s'", s->name);
		return;
	}

	dd_compute_geometry(s, mon, &x, &y, &w, &h);

	if (s->client == NULL) {
		/* First press: spawn and register hint. */
		gchar **argv;
		GError *err;
		GPid    pid;
		gboolean ok;

		if (s->pid != 0) {
			/* Remember a second press while the terminal is starting. */
			s->visible = !s->visible;
			return;
		}

		argv = NULL;
		err = NULL;
		if (!g_shell_parse_argv(s->spawn_cmd, NULL, &argv, &err)) {
			g_warning("dropdown: parse '%s': %s",
			          s->spawn_cmd, err ? err->message : "?");
			g_clear_error(&err);
			return;
		}

		pid = 0;
		ok = g_spawn_async(NULL, argv, NULL,
		                    G_SPAWN_SEARCH_PATH |
		                    G_SPAWN_DO_NOT_REAP_CHILD,
		                    NULL, NULL, &pid, &err);
		g_strfreev(argv);
		if (!ok) {
			g_warning("dropdown: spawn '%s' failed: %s",
			          s->spawn_cmd,
			          err ? err->message : "?");
			g_clear_error(&err);
			return;
		}
		s->pid = pid;
		s->visible = TRUE;
		s->child_watch = wl_event_loop_add_timer(
			gowl_compositor_get_event_loop(self->compositor), dd_child_exited, s);
		if (s->child_watch != NULL)
			wl_event_source_timer_update(s->child_watch, 50);

		gowl_compositor_prefloat_pid_with_hint(
			self->compositor,
			(pid_t)pid,
			x, y, w, h,
			GOWL_SCENE_LAYER_OVERLAY,
			dd_on_client_mapped,
			s);

		g_debug("dropdown: spawned '%s' pid=%d at %d,%d %dx%d",
		        s->name, (gint)pid, x, y, w, h);
		return;
	}

	dd_present(self, s, !s->visible);
}

/* --- GowlModule virtual methods --- */

static gboolean
dd_activate(GowlModule *mod)
{
	(void)mod;
	return TRUE;
}

static void
dd_deactivate(GowlModule *mod)
{
	GowlModuleDropdown *self = GOWL_MODULE_DROPDOWN(mod);
	guint i;
	if (self->compositor != NULL && self->focus_handler != 0)
		g_signal_handler_disconnect(self->compositor, self->focus_handler);
	self->focus_handler = 0;
	for (i = 0; i < self->entries->len; i++)
		dd_present(self, g_ptr_array_index(self->entries, i), FALSE);
}

static const gchar *
dd_get_name(GowlModule *mod)
{
	(void)mod;
	return "dropdown";
}

static const gchar *
dd_get_description(GowlModule *mod)
{
	(void)mod;
	return "Guake-style dropdown windows toggled by keybind";
}

static const gchar *
dd_get_version(GowlModule *mod)
{
	(void)mod;
	return "0.1.0";
}

/* --- GowlStartupHandler --- */

/**
 * dd_populate_entries_from_config:
 * @self: the module
 *
 * Walks the gowl config's dropdowns array and materialises a
 * #DropdownState for each entry.  Parses the keybind string via
 * gowl_keybind_parse() and logs a warning on parse failures.
 */
static void
dd_populate_entries_from_config(GowlModuleDropdown *self)
{
	GowlConfig *cfg;
	GPtrArray  *arr;
	guint       i;

	cfg = gowl_compositor_get_config(self->compositor);
	if (cfg == NULL)
		return;
	arr = gowl_config_get_dropdowns(cfg);
	if (arr == NULL || arr->len == 0) {
		g_debug("dropdown: no dropdowns in config");
		return;
	}

	for (i = 0; i < arr->len; i++) {
		GowlDropdownEntry *e;
		DropdownState     *s;

		e = (GowlDropdownEntry *)g_ptr_array_index(arr, i);
		if (e->name == NULL || e->spawn_cmd == NULL || dd_find_by_name(self, e->name) != NULL)
			continue;

		s = g_new0(DropdownState, 1);
		s->name       = g_strdup(e->name);
		s->spawn_cmd  = g_strdup(e->spawn_cmd);
		s->width_pct  = e->width_pct;
		s->height_pct = e->height_pct;
		s->width_abs  = e->width_abs;
		s->height_abs = e->height_abs;
		s->anchor     = e->anchor;
		s->module     = self;

		if (e->keybind != NULL) {
			if (!gowl_keybind_parse(e->keybind,
			                         &s->modifiers,
			                         &s->keysym)) {
				g_warning("dropdown: could not parse keybind '%s' for '%s'",
				          e->keybind, e->name);
				s->modifiers = 0;
				s->keysym = 0;
			}
		}

		g_ptr_array_add(self->entries, s);
		g_debug("dropdown: registered entry '%s'", e->name);
	}
}

static void
dd_display_destroyed(struct wl_listener *listener, void *data)
{
	GowlModuleDropdown *self = wl_container_of(listener, self, display_destroy);
	guint i;
	for (i = 0; i < self->entries->len; i++) {
		DropdownState *s = g_ptr_array_index(self->entries, i);
		if (s->child_watch != NULL) {
			wl_event_source_remove(s->child_watch);
			s->child_watch = NULL;
		}
	}
	wl_list_remove(&self->display_destroy.link);
	wl_list_init(&self->display_destroy.link);
}

static void
dd_on_startup(GowlStartupHandler *handler, gpointer compositor)
{
	GowlModuleDropdown *self;

	self = GOWL_MODULE_DROPDOWN(handler);
	if (self->compositor == NULL) {
		self->compositor = GOWL_COMPOSITOR(compositor);
		g_object_add_weak_pointer(G_OBJECT(compositor), (gpointer *)&self->compositor);
		self->display_destroy.notify = dd_display_destroyed;
		wl_display_add_destroy_listener(gowl_compositor_get_wl_display(self->compositor),
			&self->display_destroy);
	}
	if (self->focus_handler == 0)
		self->focus_handler = g_signal_connect(compositor, "focus-changed",
			G_CALLBACK(dd_focus_changed), self);
	dd_populate_entries_from_config(self);
	g_debug("dropdown: startup, %u entries", self->entries->len);
}

static void
dd_startup_init(GowlStartupHandlerInterface *iface)
{
	iface->on_startup = dd_on_startup;
}

/* --- GowlKeybindHandler --- */

static gboolean
dd_handle_key(
	GowlKeybindHandler *handler,
	guint               modifiers,
	guint               keysym,
	gboolean            pressed
){
	GowlModuleDropdown *self;
	DropdownState      *s;

	if (!pressed)
		return FALSE;

	self = GOWL_MODULE_DROPDOWN(handler);
	s = dd_find_by_key(self, modifiers, keysym);
	if (s == NULL)
		return FALSE;

	dd_entry_toggle(self, s);
	return TRUE;
}

static void
dd_keybind_init(GowlKeybindHandlerInterface *iface)
{
	iface->handle_key = dd_handle_key;
}

/* --- Public C API (used by cmacs DEFUNs) --- */

/**
 * dd_adopt_config_entry:
 * @self: the module
 * @name: entry name to look up in the compositor's config
 *
 * Fallback lookup used by gowl_module_dropdown_toggle_by_name()
 * when a user adds a dropdown at runtime (post-startup) via the
 * elisp API and then tries to toggle it before a refresh.  Scans
 * gowl_config_get_dropdowns() for a matching name, materialises
 * a new #DropdownState in the module's cache, and returns it.
 *
 * Returns: (nullable): the newly adopted state, or %NULL if no
 *          matching config entry exists.
 */
static DropdownState *
dd_adopt_config_entry(GowlModuleDropdown *self, const gchar *name)
{
	GowlConfig *cfg;
	GPtrArray  *arr;
	guint       i;

	if (self->compositor == NULL || name == NULL)
		return NULL;
	cfg = gowl_compositor_get_config(self->compositor);
	if (cfg == NULL)
		return NULL;
	arr = gowl_config_get_dropdowns(cfg);
	if (arr == NULL)
		return NULL;

	for (i = 0; i < arr->len; i++) {
		GowlDropdownEntry *e;
		DropdownState     *s;

		e = (GowlDropdownEntry *)g_ptr_array_index(arr, i);
		if (e->name == NULL || g_strcmp0(e->name, name) != 0)
			continue;

		s = g_new0(DropdownState, 1);
		s->name       = g_strdup(e->name);
		s->spawn_cmd  = g_strdup(e->spawn_cmd);
		s->width_pct  = e->width_pct;
		s->height_pct = e->height_pct;
		s->width_abs  = e->width_abs;
		s->height_abs = e->height_abs;
		s->anchor     = e->anchor;
		s->module     = self;

		if (e->keybind != NULL)
			gowl_keybind_parse(e->keybind,
			                    &s->modifiers, &s->keysym);

		g_ptr_array_add(self->entries, s);
		return s;
	}
	return NULL;
}

/* --- GowlDropdownProvider --- */

/**
 * dd_provider_toggle_by_name:
 *
 * Interface method dispatched from
 * gowl_dropdown_provider_toggle_by_name() — the entry point
 * used by the cmacs DEFUN bridge.  Looks up @name in the
 * module's own entries array first; on miss, falls back to
 * scanning the compositor config so entries added at runtime
 * via `gowl-add-dropdown' work without a manual refresh.
 */
static gboolean
dd_provider_toggle_by_name(
	GowlDropdownProvider *self_iface,
	const gchar          *name
){
	GowlModuleDropdown *self;
	DropdownState      *s;

	self = GOWL_MODULE_DROPDOWN(self_iface);
	s = dd_find_by_name(self, name);
	if (s == NULL)
		s = dd_adopt_config_entry(self, name);
	if (s == NULL)
		return FALSE;
	dd_entry_toggle(self, s);
	return TRUE;
}

/**
 * dd_provider_refresh:
 *
 * Interface method that scans the compositor config's dropdowns
 * array and adopts every entry not already cached in the module.
 * Used by cmacs-gowl-mode to push defcustom-driven dropdowns
 * into the running module after startup.  Returns the count of
 * newly-adopted entries.
 */
static guint
dd_provider_refresh(GowlDropdownProvider *self_iface)
{
	GowlModuleDropdown *self;
	GowlConfig         *cfg;
	GPtrArray          *arr;
	guint               i;
	guint               adopted;

	self = GOWL_MODULE_DROPDOWN(self_iface);
	if (self->compositor == NULL)
		return 0;
	cfg = gowl_compositor_get_config(self->compositor);
	if (cfg == NULL)
		return 0;
	arr = gowl_config_get_dropdowns(cfg);
	if (arr == NULL)
		return 0;

	adopted = 0;
	for (i = 0; i < arr->len; i++) {
		GowlDropdownEntry *e;

		e = (GowlDropdownEntry *)g_ptr_array_index(arr, i);
		if (e->name == NULL)
			continue;
		if (dd_find_by_name(self, e->name) != NULL)
			continue;
		if (dd_adopt_config_entry(self, e->name) != NULL)
			adopted++;
	}
	return adopted;
}

static void
dd_provider_init(GowlDropdownProviderInterface *iface)
{
	iface->toggle_by_name = dd_provider_toggle_by_name;
	iface->refresh        = dd_provider_refresh;
}

/* --- GObject lifecycle --- */

static void
gowl_module_dropdown_finalize(GObject *object)
{
	GowlModuleDropdown *self;

	self = GOWL_MODULE_DROPDOWN(object);
	wl_list_remove(&self->display_destroy.link);
	dd_deactivate(GOWL_MODULE(self));
	g_clear_pointer(&self->entries, g_ptr_array_unref);
	if (self->compositor != NULL)
		g_object_remove_weak_pointer(G_OBJECT(self->compositor), (gpointer *)&self->compositor);

	G_OBJECT_CLASS(gowl_module_dropdown_parent_class)->finalize(object);
}

static void
gowl_module_dropdown_class_init(GowlModuleDropdownClass *klass)
{
	GObjectClass    *object_class;
	GowlModuleClass *mod_class;

	object_class = G_OBJECT_CLASS(klass);
	mod_class = GOWL_MODULE_CLASS(klass);

	object_class->finalize = gowl_module_dropdown_finalize;

	mod_class->activate        = dd_activate;
	mod_class->deactivate      = dd_deactivate;
	mod_class->get_name        = dd_get_name;
	mod_class->get_description = dd_get_description;
	mod_class->get_version     = dd_get_version;
}

static void
gowl_module_dropdown_init(GowlModuleDropdown *self)
{
	wl_list_init(&self->display_destroy.link);
	self->compositor = NULL;
	self->entries = g_ptr_array_new_with_free_func(dd_state_free);
}

/* --- Shared-object entry point --- */

G_MODULE_EXPORT GType
gowl_module_register(void)
{
	return GOWL_TYPE_MODULE_DROPDOWN;
}
