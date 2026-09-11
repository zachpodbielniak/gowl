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
 * gowl-module-scratchpad.c - windows kept in a panel that slides up from
 * the bottom of the focused output.
 *
 * It is the quake shell from the other edge.  The panel is the dropdown's
 * size, because both come from gowl_overlay_panel_box(); it is the same
 * fixed floating overlay that never rearranges the tiles beneath it; the
 * animation module slides it the same way; and, like the dropdown, it
 * rolls away as soon as focus goes to a window outside it.  Where the
 * dropdown owns the one terminal it spawned, the scratchpad takes windows
 * that already exist -- any number of them -- and tiles them in columns
 * across the panel.  While it is up those windows have the keyboard, and
 * Super+j / Super+k cycle among them rather than stepping down onto the
 * tiles underneath, which would roll it away.
 *
 * It has no keys of its own.  Everything is a command, reached from a
 * keybind as { action: ipc_command, arg: "scratchpad-toggle" }, from the
 * IPC socket, or from an embedder through gowl_compositor_run_command():
 *
 *   scratchpad-toggle       show the panel if it is hidden, else hide it
 *   scratchpad-show         show it (when shown: re-tile and refocus)
 *   scratchpad-hide         hide it
 *   scratchpad-add [ID]     the focused window, or client ID, joins
 *   scratchpad-remove [ID]  it leaves, onto the tags in view
 *   scratchpad-status       visibility, members and settings
 *
 * The compositor does the window management.  adopt_overlay() takes a
 * window out of the layout as a hidden overlay, present_overlay() shows
 * and hides it, and release_overlay() gives it back as it was, floating
 * or tiled.  This file decides only which windows, where, and when.
 */

#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "gowl-scratchpad"

#include <glib-object.h>
#include <gmodule.h>
#include <string.h>
#include <wayland-server-core.h>

#include "module/gowl-module.h"
#include "interfaces/gowl-startup-handler.h"
#include "interfaces/gowl-ipc-handler.h"
#include "interfaces/gowl-scratchpad-handler.h"
#include "core/gowl-compositor.h"
#include "core/gowl-client.h"
#include "core/gowl-monitor.h"
#include "core/gowl-overlay-layout.h"

/**
 * GOWL_SCRATCHPAD_GROUP:
 *
 * The overlay group the scratchpad adopts windows into.  Focus-stack
 * steps stay within a group, which is what keeps Super+j / Super+k
 * inside a shown panel.  Adopted groups are never 0.
 */
#define GOWL_SCRATCHPAD_GROUP (1)

/**
 * GOWL_SCRATCHPAD_ANCHOR:
 *
 * The edge the panel attaches to and slides from: the bottom.  The
 * dropdown has the top.
 */
#define GOWL_SCRATCHPAD_ANCHOR (1)

/**
 * GOWL_SCRATCHPAD_MAX_SIZE:
 *
 * Largest accepted `width' or `height', in pixels.  The panel is clamped
 * to its output anyway; this only rejects nonsense.
 */
#define GOWL_SCRATCHPAD_MAX_SIZE (65535)

/**
 * GOWL_SCRATCHPAD_MAX_GAP:
 *
 * Largest accepted `gap' between columns, in pixels.
 */
#define GOWL_SCRATCHPAD_MAX_GAP (512)

/* Every command shares this prefix, so the namespace is the module's. */
#define SP_PREFIX "scratchpad-"

/**
 * ScratchpadMember:
 * @client: the window; a weak pointer, so %NULL once the object is gone
 * @destroy_id: the handler watching @client's "destroy"
 *
 * One window in the scratchpad.  Members are kept in joining order, which
 * is also the order of the panel's columns, left to right.
 */
typedef struct {
	GowlClient *client;
	gulong      destroy_id;
} ScratchpadMember;

#define GOWL_TYPE_MODULE_SCRATCHPAD (gowl_module_scratchpad_get_type())
G_DECLARE_FINAL_TYPE(GowlModuleScratchpad, gowl_module_scratchpad,
                     GOWL, MODULE_SCRATCHPAD, GowlModule)

/**
 * GowlModuleScratchpad:
 *
 * The scratchpad module.  @compositor, @last_focused and each member's
 * client are weak pointers; @busy is set while the module itself moves
 * focus, so its own presents are not mistaken for focus leaving the
 * panel.
 */
struct _GowlModuleScratchpad {
	GowlModule              parent_instance;

	GowlCompositor         *compositor;
	gulong                  focus_handler;
	gulong                  removed_handler;
	struct wl_listener      display_destroy;
	struct wl_event_source *retile_source;

	GPtrArray              *members;       /* ScratchpadMember *, owned */
	GowlClient             *last_focused;  /* the member to focus on show */
	gboolean                visible;
	gboolean                busy;

	gdouble                 width_pct;
	gdouble                 height_pct;
	gint                    width_abs;
	gint                    height_abs;
	gint                    gap;
};

static void sp_startup_init(GowlStartupHandlerInterface *iface);
static void sp_ipc_init(GowlIpcHandlerInterface *iface);
static void sp_handler_init(GowlScratchpadHandlerInterface *iface);

G_DEFINE_TYPE_WITH_CODE(GowlModuleScratchpad, gowl_module_scratchpad,
	GOWL_TYPE_MODULE,
	G_IMPLEMENT_INTERFACE(GOWL_TYPE_STARTUP_HANDLER, sp_startup_init)
	G_IMPLEMENT_INTERFACE(GOWL_TYPE_IPC_HANDLER, sp_ipc_init)
	G_IMPLEMENT_INTERFACE(GOWL_TYPE_SCRATCHPAD_HANDLER, sp_handler_init))

static gboolean sp_show(GowlModuleScratchpad *self);

/* --- Members --- */

/*
 * sp_member_free:
 * @data: a #ScratchpadMember
 *
 * Lets go of a member's window: its destroy handler, and the weak pointer
 * that would otherwise be written into freed memory.  The window itself
 * is left as it is -- releasing it is the caller's decision.
 */
static void
sp_member_free(gpointer data)
{
	ScratchpadMember *m;

	m = (ScratchpadMember *)data;
	if (m == NULL)
		return;
	if (m->client != NULL) {
		if (m->destroy_id != 0)
			g_signal_handler_disconnect(m->client, m->destroy_id);
		g_object_remove_weak_pointer(G_OBJECT(m->client),
		                             (gpointer *)&m->client);
	}
	g_free(m);
}

/*
 * sp_find:
 * @self: the module
 * @client: (nullable): any pointer; compared, never dereferenced
 *
 * Returns: (nullable): the member holding @client
 */
static ScratchpadMember *
sp_find(
	GowlModuleScratchpad *self,
	gconstpointer         client
){
	guint i;

	if (client == NULL)
		return NULL;
	for (i = 0; i < self->members->len; i++) {
		ScratchpadMember *m;

		m = (ScratchpadMember *)g_ptr_array_index(self->members, i);
		if (m->client == client)
			return m;
	}
	return NULL;
}

/*
 * sp_set_last_focused:
 * @self: the module
 * @client: (nullable): the member to focus when the panel next shows
 *
 * Kept as a weak pointer: the window can go at any time.
 */
static void
sp_set_last_focused(
	GowlModuleScratchpad *self,
	GowlClient           *client
){
	if (self->last_focused == client)
		return;
	if (self->last_focused != NULL)
		g_object_remove_weak_pointer(G_OBJECT(self->last_focused),
		                             (gpointer *)&self->last_focused);
	self->last_focused = client;
	if (client != NULL)
		g_object_add_weak_pointer(G_OBJECT(client),
		                          (gpointer *)&self->last_focused);
}

/* --- Showing and hiding --- */

/*
 * sp_present_member:
 * @self: the module
 * @m: the member
 * @mon: the output the panel is on
 * @px: the panel's left edge
 * @py: the panel's top edge
 * @pw: the panel's width
 * @ph: the panel's height
 * @n: how many columns the panel has
 * @index: which of them is @m's
 *
 * Shows one member in its column.  The compositor focuses it, and moves
 * it in place if it is already up on @mon rather than sliding it in.
 */
static void
sp_present_member(
	GowlModuleScratchpad *self,
	ScratchpadMember     *m,
	GowlMonitor          *mon,
	gint                  px,
	gint                  py,
	gint                  pw,
	gint                  ph,
	guint                 n,
	guint                 index
){
	gint x, y, w, h;

	if (m->client == NULL
	    || !gowl_overlay_tile_columns(px, py, pw, ph, n, index, self->gap,
	                                  &x, &y, &w, &h))
		return;
	gowl_compositor_present_overlay(self->compositor, m->client, mon,
	                                x, y, w, h, GOWL_SCRATCHPAD_ANCHOR,
	                                TRUE);
}

/*
 * sp_show:
 * @self: the module
 *
 * Presents every member in its column on the selected output.  Each
 * present focuses its window, so the member that should end up holding
 * the keyboard -- the one focused last, or else the first -- goes last.
 *
 * Returns: %TRUE if the panel is up
 */
static gboolean
sp_show(GowlModuleScratchpad *self)
{
	GowlMonitor      *mon;
	ScratchpadMember *target;
	gint              px, py, pw, ph;
	gint              ax, ay, aw, ah;
	guint             n;
	guint             i;
	guint             target_index;

	n = self->members->len;
	if (self->compositor == NULL || n == 0)
		return FALSE;
	mon = gowl_compositor_get_selected_monitor(self->compositor);
	if (mon == NULL)
		return FALSE;

	/* The same rectangle the dropdown takes, from the bottom edge. */
	gowl_monitor_get_window_area(mon, &ax, &ay, &aw, &ah);
	gowl_overlay_panel_box(ax, ay, aw, ah,
	                       self->width_pct, self->height_pct,
	                       self->width_abs, self->height_abs,
	                       GOWL_SCRATCHPAD_ANCHOR, &px, &py, &pw, &ph);

	target = sp_find(self, self->last_focused);
	if (target == NULL)
		target = (ScratchpadMember *)g_ptr_array_index(self->members, 0);
	target_index = 0;

	self->visible = TRUE;
	self->busy = TRUE;
	for (i = 0; i < n; i++) {
		ScratchpadMember *m;

		m = (ScratchpadMember *)g_ptr_array_index(self->members, i);
		if (m == target) {
			target_index = i;
			continue;
		}
		sp_present_member(self, m, mon, px, py, pw, ph, n, i);
	}
	sp_present_member(self, target, mon, px, py, pw, ph, n, target_index);
	self->busy = FALSE;
	return TRUE;
}

/*
 * sp_hide:
 * @self: the module
 *
 * Hides every member.  The one holding the keyboard goes last: hiding it
 * hands focus to the top visible window, which must not be another member
 * still waiting its turn.
 */
static void
sp_hide(GowlModuleScratchpad *self)
{
	ScratchpadMember *last;
	guint             i;

	self->visible = FALSE;
	if (self->compositor == NULL)
		return;

	last = sp_find(self, gowl_compositor_get_focused_client(self->compositor));
	if (last != NULL)
		sp_set_last_focused(self, last->client);

	self->busy = TRUE;
	for (i = 0; i < self->members->len; i++) {
		ScratchpadMember *m;

		m = (ScratchpadMember *)g_ptr_array_index(self->members, i);
		if (m != last && m->client != NULL)
			gowl_compositor_present_overlay(self->compositor, m->client,
			                                NULL, 0, 0, 0, 0,
			                                GOWL_SCRATCHPAD_ANCHOR, FALSE);
	}
	if (last != NULL && last->client != NULL)
		gowl_compositor_present_overlay(self->compositor, last->client,
		                                NULL, 0, 0, 0, 0,
		                                GOWL_SCRATCHPAD_ANCHOR, FALSE);
	self->busy = FALSE;
}

/*
 * sp_cmd_show:
 * @self: the module
 *
 * Returns: (transfer full): the reply to `scratchpad-show'
 */
static gchar *
sp_cmd_show(GowlModuleScratchpad *self)
{
	if (self->members->len == 0)
		return g_strdup("ERROR the scratchpad is empty; "
		                "add a window with scratchpad-add");
	if (!sp_show(self))
		return g_strdup("ERROR there is no output to show the "
		                "scratchpad on");
	return g_strdup_printf("OK shown %u", self->members->len);
}

/*
 * sp_toggle:
 * @self: the module
 *
 * Returns: (transfer full): the reply to `scratchpad-toggle'
 */
static gchar *
sp_toggle(GowlModuleScratchpad *self)
{
	if (self->visible) {
		sp_hide(self);
		return g_strdup("OK hidden");
	}
	return sp_cmd_show(self);
}

/* --- Windows leaving on their own --- */

/*
 * sp_retile:
 * @data: the module
 *
 * Re-presents a shown panel after a member left, so the rest close the
 * gap.  Runs from an idle source rather than inside the unmap or destroy
 * that caused it: that is the compositor's own teardown of a window, and
 * presenting windows from inside it would re-enter it.
 */
static void
sp_retile(void *data)
{
	GowlModuleScratchpad *self;

	self = (GowlModuleScratchpad *)data;
	self->retile_source = NULL;
	if (!self->visible)
		return;
	if (self->members->len == 0 || !sp_show(self))
		self->visible = FALSE;
}

/*
 * sp_schedule_retile:
 * @self: the module
 *
 * Asks for sp_retile() once the compositor is back in its event loop.
 */
static void
sp_schedule_retile(GowlModuleScratchpad *self)
{
	struct wl_event_loop *loop;

	if (self->retile_source != NULL)
		return;
	loop = self->compositor != NULL
		? gowl_compositor_get_event_loop(self->compositor) : NULL;
	if (loop == NULL) {
		/* No event loop, so no teardown in progress to wait for. */
		sp_retile(self);
		return;
	}
	self->retile_source = wl_event_loop_add_idle(loop, sp_retile, self);
}

/*
 * sp_forget:
 * @self: the module
 * @m: a member whose window unmapped or is being destroyed
 *
 * Drops the member and closes the gap it leaves.  The window itself is the
 * compositor's to tidy: it hands an adopted overlay back on unmap.
 */
static void
sp_forget(
	GowlModuleScratchpad *self,
	ScratchpadMember     *m
){
	if (self->last_focused == m->client)
		sp_set_last_focused(self, NULL);
	g_ptr_array_remove(self->members, m);
	if (self->members->len == 0)
		self->visible = FALSE;
	else if (self->visible)
		sp_schedule_retile(self);
}

static void
sp_on_member_destroy(
	GowlClient *client,
	gpointer    data
){
	GowlModuleScratchpad *self;
	ScratchpadMember     *m;

	self = GOWL_MODULE_SCRATCHPAD(data);
	m = sp_find(self, client);
	if (m != NULL)
		sp_forget(self, m);
}

/* A member unmapping leaves the scratchpad: it is about to lose its
 * scene, and if it maps again it does so as an ordinary window. */
static void
sp_on_client_removed(
	GowlCompositor *compositor,
	GObject        *client,
	gpointer        data
){
	GowlModuleScratchpad *self;
	ScratchpadMember     *m;

	(void)compositor;
	self = GOWL_MODULE_SCRATCHPAD(data);
	m = sp_find(self, client);
	if (m != NULL)
		sp_forget(self, m);
}

/* Focus landing on a member makes it the one the panel comes back to;
 * focus landing anywhere else rolls the panel away, as it does the
 * dropdown. */
static void
sp_on_focus_changed(
	GowlCompositor *compositor,
	GObject        *focused,
	gpointer        data
){
	GowlModuleScratchpad *self;
	ScratchpadMember     *m;

	(void)compositor;
	self = GOWL_MODULE_SCRATCHPAD(data);
	m = sp_find(self, focused);
	if (m != NULL) {
		sp_set_last_focused(self, m->client);
		return;
	}
	if (self->visible && !self->busy)
		sp_hide(self);
}

/* --- Joining and leaving --- */

/*
 * sp_track:
 * @self: the module
 * @client: a window the compositor has just adopted
 *
 * Makes @client a member, in the rightmost column.
 */
static void
sp_track(
	GowlModuleScratchpad *self,
	GowlClient           *client
){
	ScratchpadMember *m;

	m = g_new0(ScratchpadMember, 1);
	m->client = client;
	g_object_add_weak_pointer(G_OBJECT(client), (gpointer *)&m->client);
	m->destroy_id = g_signal_connect(client, "destroy",
	                                 G_CALLBACK(sp_on_member_destroy), self);
	g_ptr_array_add(self->members, m);
}

/*
 * sp_resolve:
 * @self: the module
 * @args: (nullable): a client ID, or nothing for the focused window
 * @error_reply: (out): the reply to send when no window is resolved
 *
 * Returns: (transfer none) (nullable): the window a command is about
 */
static GowlClient *
sp_resolve(
	GowlModuleScratchpad *self,
	const gchar          *args,
	gchar               **error_reply
){
	g_autofree gchar *word = NULL;
	GList            *l;
	guint64           id;
	gchar            *end;

	word = g_strstrip(g_strdup(args != NULL ? args : ""));
	if (*word == '\0') {
		GowlClient *focused;

		focused = gowl_compositor_get_focused_client(self->compositor);
		if (focused == NULL)
			*error_reply = g_strdup("ERROR no window is focused");
		return focused;
	}

	/* Digits only: strtoull would take "-1" and hand back a huge id. */
	end = NULL;
	id = g_ascii_strtoull(word, &end, 10);
	if (!g_ascii_isdigit(*word) || end == word || *end != '\0'
	    || id > G_MAXUINT) {
		*error_reply = g_strdup_printf("ERROR \"%s\" is not a window id",
		                               word);
		return NULL;
	}
	for (l = gowl_compositor_get_clients(self->compositor); l != NULL;
	     l = l->next) {
		if (gowl_client_get_id((GowlClient *)l->data) == (guint)id)
			return (GowlClient *)l->data;
	}
	*error_reply = g_strdup_printf("ERROR no window has id %s", word);
	return NULL;
}

/*
 * sp_cmd_add:
 * @self: the module
 * @args: (nullable): a client ID, or nothing for the focused window
 *
 * Returns: (transfer full): the reply to `scratchpad-add'
 */
static gchar *
sp_cmd_add(
	GowlModuleScratchpad *self,
	const gchar          *args
){
	GowlClient *client;
	gchar      *reply;
	gboolean    adopted;

	reply = NULL;
	client = sp_resolve(self, args, &reply);
	if (client == NULL)
		return reply;
	if (sp_find(self, client) != NULL)
		return g_strdup("ERROR that window is already in the scratchpad");

	/* Adopting hands the window's focus on, which is not focus leaving
	 * the panel. */
	self->busy = TRUE;
	adopted = gowl_compositor_adopt_overlay(self->compositor, client,
	                                        GOWL_SCRATCHPAD_GROUP);
	self->busy = FALSE;
	if (!adopted)
		return g_strdup("ERROR that window cannot join the scratchpad: "
		                "it is embedded, already an overlay, or not a "
		                "managed window");

	sp_track(self, client);
	/* It comes up focused the next time the panel does. */
	sp_set_last_focused(self, client);
	if (self->visible)
		sp_show(self);
	return g_strdup_printf("OK added %u", self->members->len);
}

/*
 * sp_cmd_remove:
 * @self: the module
 * @args: (nullable): a member's client ID, or nothing for the focused
 *   window
 *
 * The window goes back onto the tags the selected output is showing,
 * floating where it floated or tiled, and takes the keyboard.  The rest of
 * the panel rolls away first, so it lands on a desktop with nothing drawn
 * over it.
 *
 * Returns: (transfer full): the reply to `scratchpad-remove'
 */
static gchar *
sp_cmd_remove(
	GowlModuleScratchpad *self,
	const gchar          *args
){
	GowlClient       *client;
	ScratchpadMember *m;
	gchar            *reply;

	reply = NULL;
	client = sp_resolve(self, args, &reply);
	if (client == NULL)
		return reply;
	m = sp_find(self, client);
	if (m == NULL)
		return g_strdup("ERROR that window is not in the scratchpad");

	if (self->last_focused == client)
		sp_set_last_focused(self, NULL);
	g_ptr_array_remove(self->members, m);

	if (self->visible)
		sp_hide(self);
	gowl_compositor_release_overlay(self->compositor, client,
		gowl_compositor_get_selected_monitor(self->compositor));
	/* It is on the tags in view now, so showing it only focuses it. */
	gowl_compositor_show_client(self->compositor, client);
	return g_strdup_printf("OK removed %u", self->members->len);
}

/*
 * sp_cmd_status:
 * @self: the module
 *
 * One line of key=value pairs.  Fractions are printed with a '.' whatever
 * the locale, so a reader can parse them back.
 *
 * Returns: (transfer full): the reply to `scratchpad-status'
 */
static gchar *
sp_cmd_status(GowlModuleScratchpad *self)
{
	g_autoptr(GString) ids = NULL;
	gchar               wpct[G_ASCII_DTOSTR_BUF_SIZE];
	gchar               hpct[G_ASCII_DTOSTR_BUF_SIZE];
	guint               i;

	ids = g_string_new(NULL);
	for (i = 0; i < self->members->len; i++) {
		ScratchpadMember *m;

		m = (ScratchpadMember *)g_ptr_array_index(self->members, i);
		if (m->client == NULL)
			continue;
		if (ids->len > 0)
			g_string_append_c(ids, ',');
		g_string_append_printf(ids, "%u", gowl_client_get_id(m->client));
	}
	g_ascii_formatd(wpct, sizeof(wpct), "%g", self->width_pct);
	g_ascii_formatd(hpct, sizeof(hpct), "%g", self->height_pct);
	return g_strdup_printf("OK visible=%d count=%u members=%s "
	                       "width-pct=%s height-pct=%s width=%d height=%d "
	                       "gap=%d",
	                       self->visible ? 1 : 0, self->members->len,
	                       ids->str, wpct, hpct, self->width_abs,
	                       self->height_abs, self->gap);
}

/* --- GowlIpcHandler --- */

/*
 * sp_handle_command:
 * @handler: the module
 * @command: the command word
 * @args: (nullable): the rest of the line
 *
 * Claims every `scratchpad-' word and nothing else.
 *
 * Returns: (transfer full) (nullable): "OK ..." or "ERROR ...", or %NULL
 *   for a command that is not the scratchpad's
 */
static gchar *
sp_handle_command(
	GowlIpcHandler *handler,
	const gchar    *command,
	const gchar    *args
){
	GowlModuleScratchpad *self;
	const gchar          *verb;

	if (command == NULL || !g_str_has_prefix(command, SP_PREFIX))
		return NULL;

	self = GOWL_MODULE_SCRATCHPAD(handler);
	verb = command + strlen(SP_PREFIX);

	if (g_strcmp0(verb, "status") == 0)
		return sp_cmd_status(self);
	if (g_strcmp0(verb, "toggle") != 0 && g_strcmp0(verb, "show") != 0
	    && g_strcmp0(verb, "hide") != 0 && g_strcmp0(verb, "add") != 0
	    && g_strcmp0(verb, "remove") != 0)
		return g_strdup_printf("ERROR unknown command %s; the scratchpad "
		                       "knows " SP_PREFIX "toggle, -show, -hide, "
		                       "-add, -remove and -status", command);
	if (self->compositor == NULL)
		return g_strdup("ERROR the scratchpad has not started");

	if (g_strcmp0(verb, "toggle") == 0)
		return sp_toggle(self);
	if (g_strcmp0(verb, "show") == 0)
		return sp_cmd_show(self);
	if (g_strcmp0(verb, "add") == 0)
		return sp_cmd_add(self, args);
	if (g_strcmp0(verb, "remove") == 0)
		return sp_cmd_remove(self, args);
	sp_hide(self);
	return g_strdup("OK hidden");
}

static void
sp_ipc_init(GowlIpcHandlerInterface *iface)
{
	iface->handle_command = sp_handle_command;
}

/* --- GowlScratchpadHandler --- */

static gboolean
sp_is_scratchpad(
	GowlScratchpadHandler *handler,
	gpointer               client
){
	return sp_find(GOWL_MODULE_SCRATCHPAD(handler), client) != NULL;
}

/* @name is ignored: there is one scratchpad. */
static void
sp_toggle_scratchpad(
	GowlScratchpadHandler *handler,
	const gchar           *name
){
	GowlModuleScratchpad *self;

	(void)name;
	self = GOWL_MODULE_SCRATCHPAD(handler);
	if (self->compositor != NULL)
		g_free(sp_toggle(self));
}

static void
sp_handler_init(GowlScratchpadHandlerInterface *iface)
{
	iface->is_scratchpad     = sp_is_scratchpad;
	iface->toggle_scratchpad = sp_toggle_scratchpad;
}

/* --- Settings --- */

/*
 * sp_parse_fraction:
 * @key: the setting's name, for the warning
 * @text: its value
 * @out: (inout): set when @text is a fraction in (0, 1]; else kept
 *
 * Returns: %TRUE if @out was set
 */
static gboolean
sp_parse_fraction(
	const gchar *key,
	const gchar *text,
	gdouble     *out
){
	g_autofree gchar *copy = NULL;
	gchar            *end;
	gdouble           value;

	copy = g_strstrip(g_strdup(text));
	end = NULL;
	value = g_ascii_strtod(copy, &end);
	/* Written so that NaN fails it too. */
	if (end == copy || *end != '\0' || !(value > 0.0 && value <= 1.0)) {
		g_warning("scratchpad: %s must be a fraction greater than 0 and "
		          "at most 1, not \"%s\"; keeping the current value",
		          key, text);
		return FALSE;
	}
	*out = value;
	return TRUE;
}

/*
 * sp_parse_pixels:
 * @key: the setting's name, for the warning
 * @text: its value
 * @max: the largest accepted value
 * @out: (inout): set when @text is a whole number in [0, @max]; else kept
 *
 * Returns: %TRUE if @out was set
 */
static gboolean
sp_parse_pixels(
	const gchar *key,
	const gchar *text,
	gint         max,
	gint        *out
){
	g_autofree gchar *copy = NULL;
	gchar            *end;
	gint64            value;

	copy = g_strstrip(g_strdup(text));
	end = NULL;
	value = g_ascii_strtoll(copy, &end, 10);
	if (end == copy || *end != '\0' || value < 0 || value > max) {
		g_warning("scratchpad: %s must be a whole number of pixels from "
		          "0 to %d, not \"%s\"; keeping the current value",
		          key, max, text);
		return FALSE;
	}
	*out = (gint)value;
	return TRUE;
}

/*
 * sp_configure:
 * @mod: the module
 * @config: a #GHashTable of string settings
 *
 * width-pct and height-pct are fractions of the output's usable area;
 * width and height, when not 0, are pixels and win over them -- exactly
 * the dropdown's settings.  gap is the space between columns.  A bad
 * value is refused with a warning and the old one kept.  A shown panel
 * takes the new size at once.
 */
static void
sp_configure(
	GowlModule *mod,
	gpointer    config
){
	GowlModuleScratchpad *self;
	GHashTable           *settings;
	const gchar          *value;

	self = GOWL_MODULE_SCRATCHPAD(mod);
	settings = (GHashTable *)config;
	if (settings == NULL)
		return;

	if ((value = g_hash_table_lookup(settings, "width-pct")) != NULL)
		sp_parse_fraction("width-pct", value, &self->width_pct);
	if ((value = g_hash_table_lookup(settings, "height-pct")) != NULL)
		sp_parse_fraction("height-pct", value, &self->height_pct);
	if ((value = g_hash_table_lookup(settings, "width")) != NULL)
		sp_parse_pixels("width", value, GOWL_SCRATCHPAD_MAX_SIZE,
		                &self->width_abs);
	if ((value = g_hash_table_lookup(settings, "height")) != NULL)
		sp_parse_pixels("height", value, GOWL_SCRATCHPAD_MAX_SIZE,
		                &self->height_abs);
	if ((value = g_hash_table_lookup(settings, "gap")) != NULL)
		sp_parse_pixels("gap", value, GOWL_SCRATCHPAD_MAX_GAP, &self->gap);

	if (self->visible)
		sp_show(self);
}

/* --- GowlModule virtual methods --- */

/*
 * sp_release_all:
 * @self: the module
 *
 * Gives every member back to the layout, on the tags in view.  This is
 * what switching the module off does, so no window is ever left as a
 * hidden overlay with nobody to show it.
 */
static void
sp_release_all(GowlModuleScratchpad *self)
{
	g_autoptr(GPtrArray) members = NULL;
	GowlMonitor         *mon;
	guint                i;

	self->visible = FALSE;
	sp_set_last_focused(self, NULL);
	if (self->members->len == 0)
		return;

	/* Detach the list first.  Releasing re-arranges and refocuses, and
	 * the handlers that fires must find nothing left to act on. */
	members = self->members;
	self->members = g_ptr_array_new_with_free_func(sp_member_free);

	mon = self->compositor != NULL
		? gowl_compositor_get_selected_monitor(self->compositor) : NULL;
	for (i = 0; i < members->len; i++) {
		ScratchpadMember *m;

		m = (ScratchpadMember *)g_ptr_array_index(members, i);
		if (m->client != NULL && self->compositor != NULL)
			gowl_compositor_release_overlay(self->compositor, m->client,
			                                mon);
	}
}

static gboolean
sp_activate(GowlModule *mod)
{
	(void)mod;
	return TRUE;
}

static void
sp_deactivate(GowlModule *mod)
{
	GowlModuleScratchpad *self;

	self = GOWL_MODULE_SCRATCHPAD(mod);
	if (self->compositor != NULL) {
		if (self->focus_handler != 0)
			g_signal_handler_disconnect(self->compositor,
			                            self->focus_handler);
		if (self->removed_handler != 0)
			g_signal_handler_disconnect(self->compositor,
			                            self->removed_handler);
	}
	self->focus_handler = 0;
	self->removed_handler = 0;
	if (self->retile_source != NULL) {
		wl_event_source_remove(self->retile_source);
		self->retile_source = NULL;
	}
	sp_release_all(self);
}

static const gchar *
sp_get_name(GowlModule *mod)
{
	(void)mod;
	return "scratchpad";
}

static const gchar *
sp_get_description(GowlModule *mod)
{
	(void)mod;
	return "A panel of windows that slides up from the bottom";
}

static const gchar *
sp_get_version(GowlModule *mod)
{
	(void)mod;
	return "0.2.0";
}

/* --- GowlStartupHandler --- */

/* The idle source a retile waits on belongs to the display's event loop,
 * which goes with the display. */
static void
sp_display_destroyed(
	struct wl_listener *listener,
	void               *data
){
	GowlModuleScratchpad *self;

	(void)data;
	self = wl_container_of(listener, self, display_destroy);
	if (self->retile_source != NULL) {
		wl_event_source_remove(self->retile_source);
		self->retile_source = NULL;
	}
	wl_list_remove(&self->display_destroy.link);
	wl_list_init(&self->display_destroy.link);
}

static void
sp_on_startup(
	GowlStartupHandler *handler,
	gpointer            compositor
){
	GowlModuleScratchpad *self;
	struct wl_display    *display;

	self = GOWL_MODULE_SCRATCHPAD(handler);
	if (self->compositor == NULL) {
		self->compositor = GOWL_COMPOSITOR(compositor);
		g_object_add_weak_pointer(G_OBJECT(compositor),
		                          (gpointer *)&self->compositor);
	}

	/* A compositor that never started (the unit tests) has no display. */
	display = gowl_compositor_get_wl_display(self->compositor);
	if (display != NULL && wl_list_empty(&self->display_destroy.link))
		wl_display_add_destroy_listener(display, &self->display_destroy);

	if (self->focus_handler == 0)
		self->focus_handler = g_signal_connect(self->compositor,
			"focus-changed", G_CALLBACK(sp_on_focus_changed), self);
	if (self->removed_handler == 0)
		self->removed_handler = g_signal_connect(self->compositor,
			"client-removed", G_CALLBACK(sp_on_client_removed), self);
}

static void
sp_startup_init(GowlStartupHandlerInterface *iface)
{
	iface->on_startup = sp_on_startup;
}

/* --- GObject lifecycle --- */

static void
gowl_module_scratchpad_finalize(GObject *object)
{
	GowlModuleScratchpad *self;

	self = GOWL_MODULE_SCRATCHPAD(object);
	wl_list_remove(&self->display_destroy.link);
	sp_deactivate(GOWL_MODULE(self));
	g_clear_pointer(&self->members, g_ptr_array_unref);
	if (self->compositor != NULL)
		g_object_remove_weak_pointer(G_OBJECT(self->compositor),
		                             (gpointer *)&self->compositor);

	G_OBJECT_CLASS(gowl_module_scratchpad_parent_class)->finalize(object);
}

static void
gowl_module_scratchpad_class_init(GowlModuleScratchpadClass *klass)
{
	GObjectClass    *object_class;
	GowlModuleClass *module_class;

	object_class = G_OBJECT_CLASS(klass);
	module_class = GOWL_MODULE_CLASS(klass);

	object_class->finalize = gowl_module_scratchpad_finalize;

	module_class->activate        = sp_activate;
	module_class->deactivate      = sp_deactivate;
	module_class->get_name        = sp_get_name;
	module_class->get_description = sp_get_description;
	module_class->get_version     = sp_get_version;
	module_class->configure       = sp_configure;
}

static void
gowl_module_scratchpad_init(GowlModuleScratchpad *self)
{
	wl_list_init(&self->display_destroy.link);
	self->display_destroy.notify = sp_display_destroyed;
	self->members = g_ptr_array_new_with_free_func(sp_member_free);
	self->width_pct = GOWL_OVERLAY_DEFAULT_WIDTH_PCT;
	self->height_pct = GOWL_OVERLAY_DEFAULT_HEIGHT_PCT;
}

/* --- Shared-object entry point --- */

G_MODULE_EXPORT GType
gowl_module_register(void)
{
	return GOWL_TYPE_MODULE_SCRATCHPAD;
}
