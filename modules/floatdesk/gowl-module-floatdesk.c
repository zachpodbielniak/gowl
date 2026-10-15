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
 * gowl-module-floatdesk.c - a floating desktop, per workspace.
 *
 * A workspace (tag) can be put in "floating desktop" mode: every window
 * on it floats, new windows open centred at a sensible size and cascade,
 * floating windows get title bars (minimize, maximize, close; drag to an
 * edge to snap), focus follows the click, and a dock along one edge of
 * the output lists pinned and running applications.  Leaving the mode
 * tiles again exactly the windows the mode floated, and nothing else.
 *
 * The mode is one entry in the layout cycle (`desktop'), so the usual
 * layout keys reach it, and it has its own toggle.  Which workspaces are
 * floating lives in THIS module's state -- gowl_floatdesk_state_*() --
 * and is saved to $XDG_STATE_HOME/gowl/floatdesk.yaml.  The layout name
 * is only a projection of it: the layout registry has no idea "desktop"
 * is a mode, and reading the mode back out of it would lose the
 * previous layout, the default, and every workspace not in view.
 *
 * Everything that changes which windows float happens in one place, the
 * compositor's `pre-arrange' signal, at the start of every arrange and
 * before the layout runs.  That covers every way a window reaches or
 * leaves a workspace -- mapped, moved by tag, by output, by rule, by a
 * restore -- without a hook per path: whichever path it was, it ends in
 * an arrange.
 *
 * Commands (the IPC socket, a keybind's ipc_command, an embedder's
 * gowl_compositor_run_command):
 *
 *   floatdesk-toggle [TAG]          the view (or TAG) floats / tiles
 *   floatdesk-on [TAG] | -off [TAG] explicitly
 *   floatdesk-default on|off|toggle float every workspace not set
 *   floatdesk-status                one line of key=value
 *   floatdesk-minimize [ID]         the focused window (or ID)
 *   floatdesk-restore [ID]          the last minimized (or ID)
 *   floatdesk-maximize [ID]         toggle
 *   floatdesk-snap left|right|max [ID]
 *   floatdesk-dock-position bottom|left|right|top
 *   floatdesk-dock-autohide on|off|toggle
 *   floatdesk-dock-pin APP [INDEX] | floatdesk-dock-unpin APP
 *   floatdesk-dock-launch APP | floatdesk-dock-activate APP
 *   floatdesk-dock-apps             the all-apps grid
 *   floatdesk-dock-status
 */

#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "gowl-floatdesk"

#include <gmodule.h>
#include <stdarg.h>
#include <string.h>
#include <wlr/util/box.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_xdg_shell.h>

#include "interfaces/gowl-startup-handler.h"
#include "interfaces/gowl-shutdown-handler.h"
#include "interfaces/gowl-ipc-handler.h"
#include "interfaces/gowl-mouse-handler.h"
#include "interfaces/gowl-keybind-handler.h"
#include "interfaces/gowl-focus-policy.h"
#include "interfaces/gowl-scene-effect.h"
#include "core/gowl-compositor.h"
#include "core/gowl-client.h"
#include "core/gowl-monitor.h"
#include "core/gowl-layout-registry.h"
#include "module/gowl-module-manager.h"
#include "config/gowl-config.h"

#include "gowl-floatdesk-private.h"

/* Defaults for the settings, documented in docs/floatdesk.org. */
#define FLOATDESK_DEFAULT_WIDTH_PCT   (0.6)
#define FLOATDESK_DEFAULT_HEIGHT_PCT  (0.7)
#define FLOATDESK_DEFAULT_CASCADE     (32)
#define FLOATDESK_DEFAULT_TITLEBAR    (28)
#define FLOATDESK_DEFAULT_SNAP        (6)
#define FLOATDESK_DEFAULT_ICON_SIZE   (48)
#define FLOATDESK_DEFAULT_TERMINAL    "foot"
#define FLOATDESK_SYMBOL              "[D]"

/* Monotonic minimize order, so `floatdesk-restore' with no argument
 * brings back the window minimized LAST. */
#define FLOATDESK_MIN_SEQ_KEY "gowl-floatdesk-min-seq"

static void floatdesk_startup_init  (GowlStartupHandlerInterface  *iface);
static void floatdesk_shutdown_init (GowlShutdownHandlerInterface *iface);
static void floatdesk_ipc_init      (GowlIpcHandlerInterface      *iface);
static void floatdesk_mouse_init    (GowlMouseHandlerInterface    *iface);
static void floatdesk_keybind_init  (GowlKeybindHandlerInterface  *iface);
static void floatdesk_focus_init    (GowlFocusPolicyInterface     *iface);
static void floatdesk_effect_init   (GowlSceneEffectInterface     *iface);

G_DEFINE_TYPE_WITH_CODE(GowlModuleFloatdesk, gowl_module_floatdesk,
	GOWL_TYPE_MODULE,
	G_IMPLEMENT_INTERFACE(GOWL_TYPE_STARTUP_HANDLER, floatdesk_startup_init)
	G_IMPLEMENT_INTERFACE(GOWL_TYPE_SHUTDOWN_HANDLER, floatdesk_shutdown_init)
	G_IMPLEMENT_INTERFACE(GOWL_TYPE_IPC_HANDLER, floatdesk_ipc_init)
	G_IMPLEMENT_INTERFACE(GOWL_TYPE_MOUSE_HANDLER, floatdesk_mouse_init)
	G_IMPLEMENT_INTERFACE(GOWL_TYPE_KEYBIND_HANDLER, floatdesk_keybind_init)
	G_IMPLEMENT_INTERFACE(GOWL_TYPE_FOCUS_POLICY, floatdesk_focus_init)
	G_IMPLEMENT_INTERFACE(GOWL_TYPE_SCENE_EFFECT, floatdesk_effect_init))

/* ================================================================ */
/* Small helpers                                                     */
/* ================================================================ */

/*
 * floatdesk_active:
 * @self: the module
 *
 * Returns: %TRUE when the module is switched on and has a compositor
 */
static gboolean
floatdesk_active(GowlModuleFloatdesk *self)
{
	return self->compositor != NULL
	       && gowl_module_get_is_active(GOWL_MODULE(self));
}

/*
 * mon_track:
 * @self: the module
 * @m: an output
 *
 * Returns: (transfer none): what the module remembers about @m, created
 *   on first use
 */
static FloatdeskMonitor *
mon_track(
	GowlModuleFloatdesk *self,
	GowlMonitor         *m
){
	FloatdeskMonitor *t;

	t = g_hash_table_lookup(self->monitors, m);
	if (t == NULL) {
		t = g_new0(FloatdeskMonitor, 1);
		g_hash_table_insert(self->monitors, m, t);
	}
	return t;
}

static void
mon_track_free(gpointer data)
{
	FloatdeskMonitor *t = data;

	g_free(t->layout);
	g_free(t);
}

gboolean
floatdesk_client_on_floating(
	GowlModuleFloatdesk *self,
	GowlClient          *c
){
	return c != NULL && c->tags != 0
	       && gowl_floatdesk_state_mask_floating(self->state, c->tags);
}

gboolean
floatdesk_view_floating(
	GowlModuleFloatdesk *self,
	GowlMonitor         *m
){
	return m != NULL && gowl_floatdesk_state_mask_floating(self->state,
		m->tagset[m->seltags]);
}

void
floatdesk_toast(
	GowlModuleFloatdesk *self,
	GowlMonitor         *m,
	const gchar         *text
){
	if (self->compositor == NULL)
		return;
	if (m == NULL)
		m = self->compositor->selmon;
	if (m == NULL)
		return;
	/* One toast surface, drawn by layout-indicator: a new message
	 * replaces the one on screen rather than stacking under it. */
	g_signal_emit_by_name(self->compositor, "toast-requested", m, text);
}

/*
 * floatdesk_event:
 * @self: the module
 * @format: what happened, printf-style
 *
 * Announces a change of state as the compositor's `module-event' (and
 * `EVENT floatdesk ...' on the IPC socket): "mode 3 floating",
 * "default tiled", "dock position left".  cmacs bridges it to
 * `cmacs-gowl-module-event-functions' and D-Bus ModuleEvent.
 */
static void
floatdesk_event(
	GowlModuleFloatdesk *self,
	const gchar         *format,
	...
) G_GNUC_PRINTF(2, 3);

static void
floatdesk_event(
	GowlModuleFloatdesk *self,
	const gchar         *format,
	...
){
	g_autofree gchar *text = NULL;
	va_list           ap;

	if (self->compositor == NULL)
		return;
	va_start(ap, format);
	text = g_strdup_vprintf(format, ap);
	va_end(ap);
	gowl_compositor_emit_module_event(self->compositor, "floatdesk", "%s",
	                                  text);
}

gboolean
floatdesk_save(GowlModuleFloatdesk *self)
{
	GError *error = NULL;

	if (self->state_path == NULL)
		return TRUE;
	if (gowl_floatdesk_state_save(self->state, self->state_path, &error))
		return TRUE;

	/* Loud: a mode that will not survive a restart is a mode the user
	 * should know about now, not discover tomorrow. */
	g_warning("floatdesk: could not save %s: %s", self->state_path,
	          error->message);
	{
		g_autofree gchar *msg = g_strdup_printf(
			"Floating desktop: could not save state (%s)",
			error->message);
		floatdesk_toast(self, NULL, msg);
	}
	g_error_free(error);
	return FALSE;
}

/*
 * mask_text:
 * @mask: a tag mask
 *
 * Returns: (transfer full): "1,3,4" -- 1-based tag numbers
 */
static gchar *
mask_text(guint32 mask)
{
	GString *out = g_string_new(NULL);
	gint     i;

	for (i = 0; i < GOWL_FLOATDESK_MAX_TAGS; i++) {
		if ((mask & ((guint32)1u << i)) == 0)
			continue;
		if (out->len > 0)
			g_string_append_c(out, ',');
		g_string_append_printf(out, "%d", i + 1);
	}
	return g_string_free(out, FALSE);
}

/*
 * find_client:
 * @self: the module
 * @args: (nullable): a client id, or nothing for the focused window
 * @error_out: (out): an error reply when there is no such window
 *
 * Returns: (transfer none) (nullable): the window
 */
static GowlClient *
find_client(
	GowlModuleFloatdesk *self,
	const gchar         *args,
	gchar              **error_out
){
	g_autofree gchar *copy = NULL;
	GList            *l;
	gchar            *end;
	guint64           id;

	*error_out = NULL;
	copy = g_strstrip(g_strdup(args != NULL ? args : ""));
	if (*copy == '\0') {
		GowlClient *c = focustop_public(self->compositor,
		                                self->compositor->selmon);

		if (c == NULL)
			*error_out = g_strdup("ERROR no focused window");
		return c;
	}
	id = g_ascii_strtoull(copy, &end, 10);
	if (end == copy || *end != '\0') {
		*error_out = g_strdup_printf("ERROR \"%s\" is not a client id",
		                             copy);
		return NULL;
	}
	for (l = self->compositor->clients; l != NULL; l = l->next) {
		GowlClient *c = l->data;

		if ((guint64)c->id == id)
			return c;
	}
	*error_out = g_strdup_printf("ERROR no client %s", copy);
	return NULL;
}

/* ================================================================ */
/* Maximize and snap                                                 */
/* ================================================================ */

gboolean
floatdesk_is_maximized(GowlClient *c)
{
	FloatdeskRestore *r;

	r = g_object_get_data(G_OBJECT(c), FLOATDESK_RESTORE_KEY);
	return r != NULL && r->kind == GOWL_FLOATDESK_SNAP_MAXIMIZE;
}

/*
 * snap_box:
 * @m: the output
 * @kind: where to
 * @out: (out): the frame
 */
static void
snap_box(
	GowlMonitor       *m,
	GowlFloatdeskSnap  kind,
	struct wlr_box    *out
){
	*out = m->w;
	if (kind == GOWL_FLOATDESK_SNAP_LEFT) {
		out->width = m->w.width / 2;
	} else if (kind == GOWL_FLOATDESK_SNAP_RIGHT) {
		out->width = m->w.width - m->w.width / 2;
		out->x = m->w.x + m->w.width / 2;
	}
}

void
floatdesk_snap(
	GowlModuleFloatdesk *self,
	GowlClient          *c,
	GowlFloatdeskSnap    kind
){
	FloatdeskRestore *r;
	struct wlr_box    box;

	if (c == NULL || c->mon == NULL || kind == GOWL_FLOATDESK_SNAP_NONE
	    || !c->isfloating || c->isfullscreen)
		return;

	/* Remember where it was only the first time: snapping a snapped
	 * window to the other half must still restore the original. */
	r = g_object_get_data(G_OBJECT(c), FLOATDESK_RESTORE_KEY);
	if (r == NULL) {
		r = g_new0(FloatdeskRestore, 1);
		r->box = c->geom;
		g_object_set_data_full(G_OBJECT(c), FLOATDESK_RESTORE_KEY, r,
		                       g_free);
	}
	r->kind = kind;
	snap_box(c->mon, kind, &box);
	gowl_compositor_place_client(self->compositor, c, box.x, box.y,
	                             box.width, box.height);
	wlr_scene_node_raise_to_top(&c->scene->node);
	floatdesk_titlebar_sync(self, c);
	floatdesk_dock_refresh(self, c->mon);
}

void
floatdesk_unsnap(
	GowlModuleFloatdesk *self,
	GowlClient          *c
){
	FloatdeskRestore *r;
	struct wlr_box    box;

	if (c == NULL)
		return;
	r = g_object_get_data(G_OBJECT(c), FLOATDESK_RESTORE_KEY);
	if (r == NULL)
		return;
	box = r->box;
	g_object_set_data(G_OBJECT(c), FLOATDESK_RESTORE_KEY, NULL);
	if (c->mon == NULL || !c->isfloating || c->isfullscreen)
		return;
	gowl_compositor_place_client(self->compositor, c, box.x, box.y,
	                             box.width, box.height);
	floatdesk_titlebar_sync(self, c);
	floatdesk_dock_refresh(self, c->mon);
}

void
floatdesk_maximize_toggle(
	GowlModuleFloatdesk *self,
	GowlClient          *c
){
	if (c == NULL)
		return;
	if (floatdesk_is_maximized(c))
		floatdesk_unsnap(self, c);
	else
		floatdesk_snap(self, c, GOWL_FLOATDESK_SNAP_MAXIMIZE);
}

/*
 * release_maximized:
 * @self: the module
 * @m: the output
 * @tags: the workspace a window just opened on
 * @except: the window that opened
 *
 * A maximized window owns its workspace until another window opens
 * there; then it is restored, so the new window is not opened BEHIND a
 * window covering the whole screen.
 */
static void
release_maximized(
	GowlModuleFloatdesk *self,
	GowlMonitor         *m,
	guint32              tags,
	GowlClient          *except
){
	GList *l;

	for (l = self->compositor->clients; l != NULL; l = l->next) {
		GowlClient *o = l->data;

		if (o == except || o->mon != m || (o->tags & tags) == 0
		    || !floatdesk_is_maximized(o))
			continue;
		floatdesk_unsnap(self, o);
	}
}

/* ================================================================ */
/* The mode: layout and state kept in step                           */
/* ================================================================ */

/*
 * first_tiling_layout:
 * @self: the module
 *
 * Returns: (transfer none) (nullable): the first registered layout that
 *   is not the mode's -- where a workspace with no remembered layout
 *   goes back to
 */
static const gchar *
first_tiling_layout(GowlModuleFloatdesk *self)
{
	GList       *names, *l;
	const gchar *out = NULL;

	names = gowl_layout_list(self->compositor);
	for (l = names; l != NULL; l = l->next) {
		if (g_strcmp0(l->data, GOWL_FLOATDESK_LAYOUT) != 0) {
			out = l->data;
			break;
		}
	}
	g_list_free(names);
	return out;
}

/*
 * leave_layout_for:
 * @self: the module
 * @mask: the workspace
 *
 * Returns: (transfer none) (nullable): the layout @mask goes back to
 *   when it stops floating
 */
static const gchar *
leave_layout_for(
	GowlModuleFloatdesk *self,
	guint32              mask
){
	const gchar *prev;

	prev = gowl_floatdesk_state_previous_layout_for_mask(self->state, mask);
	if (prev != NULL && gowl_layout_lookup(self->compositor, prev) != NULL)
		return prev;
	return first_tiling_layout(self);
}

/*
 * remember_previous:
 * @self: the module
 * @mask: the workspace about to float
 * @layout: (nullable): the layout it has now
 */
static void
remember_previous(
	GowlModuleFloatdesk *self,
	guint32              mask,
	const gchar         *layout
){
	guint tag;

	if (layout == NULL || g_strcmp0(layout, GOWL_FLOATDESK_LAYOUT) == 0)
		return;
	for (tag = 0; tag < GOWL_FLOATDESK_MAX_TAGS; tag++)
		if ((mask & ((guint32)1u << tag)) != 0)
			gowl_floatdesk_state_set_previous_layout(self->state,
			                                         tag, layout);
}

/*
 * set_monitor_layout:
 * @m: the output
 * @name: the layout
 *
 * Names @m's layout directly.  Only for use inside a pre-arrange: the
 * arrange that follows runs it, and gowl_layout_set() would arrange a
 * second time from inside the first.
 */
static void
set_monitor_layout(
	GowlMonitor *m,
	const gchar *name
){
	if (name == NULL || g_strcmp0(m->layout_name, name) == 0)
		return;
	g_free(m->layout_name);
	m->layout_name = g_strdup(name);
}

/*
 * sync_layout:
 * @self: the module
 * @m: the output being arranged
 *
 * The one place the layout and the mode are reconciled.  Four cases,
 * told apart by what the previous arrange of @m saw:
 *
 * - the layout became `desktop' on the SAME view: the user picked it
 *   (the layout cycle, `set-layout desktop'), so the view enters the
 *   mode, remembering the layout it came from;
 * - the layout stopped being `desktop' on the same view: the user left
 *   it, so the view leaves the mode;
 * - otherwise the mode is authoritative: a tag switch onto a floating
 *   workspace whose remembered layout is not `desktop' (a restart, the
 *   default turned on, a tag set floating while not in view) gets the
 *   layout; one onto a tiling workspace still showing `desktop' gets
 *   its previous layout back.
 *
 * Reading the mode out of the layout instead would lose every workspace
 * not on screen.
 */
static void
sync_layout(
	GowlModuleFloatdesk *self,
	GowlMonitor         *m
){
	FloatdeskMonitor *t;
	GowlLayoutEntry  *entry;
	const gchar      *name;
	guint32           mask;
	gboolean          floating, is_desk, same_view, was_desk;

	if (gowl_layout_lookup(self->compositor, GOWL_FLOATDESK_LAYOUT) == NULL)
		return;

	mask = m->tagset[m->seltags];
	entry = gowl_layout_get(self->compositor, m);
	name = entry != NULL ? entry->name : NULL;
	t = mon_track(self, m);
	floating = gowl_floatdesk_state_mask_floating(self->state, mask);
	is_desk = g_strcmp0(name, GOWL_FLOATDESK_LAYOUT) == 0;
	same_view = t->valid && t->mask == mask;
	was_desk = same_view && g_strcmp0(t->layout, GOWL_FLOATDESK_LAYOUT) == 0;

	if (mask != 0 && is_desk && !floating) {
		if (same_view && !was_desk) {
			/* Entered from the layout cycle. */
			g_autofree gchar *tags = mask_text(mask);

			remember_previous(self, mask, t->layout);
			gowl_floatdesk_state_set_mask(self->state, mask, TRUE);
			floatdesk_save(self);
			floatdesk_event(self, "mode %s floating", tags);
		} else {
			name = leave_layout_for(self, mask);
			set_monitor_layout(m, name);
		}
	} else if (mask != 0 && !is_desk && floating) {
		if (was_desk) {
			/* Left through the layout cycle. */
			g_autofree gchar *tags = mask_text(mask);

			gowl_floatdesk_state_set_mask(self->state, mask, FALSE);
			floatdesk_save(self);
			floatdesk_event(self, "mode %s tiled", tags);
		} else {
			remember_previous(self, mask, name);
			set_monitor_layout(m, GOWL_FLOATDESK_LAYOUT);
			name = GOWL_FLOATDESK_LAYOUT;
		}
	}

	t->valid = TRUE;
	t->mask = mask;
	g_free(t->layout);
	t->layout = g_strdup(name);
}

/* ================================================================ */
/* Reconcile: which windows float                                    */
/* ================================================================ */

/*
 * client_view:
 * @c: a window
 * @out: (out): its flags, for gowl_floatdesk_state_decide()
 */
static void
client_view(
	GowlClient              *c,
	GowlFloatdeskClientView *out
){
	memset(out, 0, sizeof(*out));
	out->tags = c->tags;
	out->floating = c->isfloating;
	out->fullscreen = c->isfullscreen;
	out->mode_floated = c->mode_floated;
	out->overlay = c->isoverlay;
	out->unmanaged = c->isunmanaged;
	out->embedded = c->isembedded;
	out->minimized = c->isminimized;
}

/*
 * place_fresh:
 * @self: the module
 * @m: its output
 * @c: a window that has just mapped onto a floating workspace
 *
 * The default size (a fraction of the usable area, held to what the
 * client says it can be), centred for the first window and cascading
 * from the last one opened after that.  Done in the same arrange that
 * floats it, so its first configure is already this size: there is no
 * frame where it was a tile.
 */
static void
place_fresh(
	GowlModuleFloatdesk *self,
	GowlMonitor         *m,
	GowlClient          *c
){
	FloatdeskMonitor *t;
	gint              width, height, x, y, extra;

	t = mon_track(self, m);
	width = (gint)(m->w.width * self->width_pct);
	height = (gint)(m->w.height * self->height_pct);

	/* The client's own limits are on the CONTENT; the frame adds the
	 * border and the title bar. */
	extra = 2 * (gint)c->bw + (self->titlebars ? self->titlebar_height : 0);
	if (c->xdg_toplevel != NULL) {
		struct wlr_xdg_toplevel_state *s = &c->xdg_toplevel->current;

		if (s->min_width > 0)
			width = MAX(width, s->min_width + 2 * (gint)c->bw);
		if (s->min_height > 0)
			height = MAX(height, s->min_height + extra);
		if (s->max_width > 0)
			width = MIN(width, s->max_width + 2 * (gint)c->bw);
		if (s->max_height > 0)
			height = MIN(height, s->max_height + extra);
	}
	width = CLAMP(width, 64, MAX(64, m->w.width));
	height = CLAMP(height, 48, MAX(48, m->w.height));

	gowl_floatdesk_place_cascade(m->w.x, m->w.y, m->w.width, m->w.height,
	                             width, height, t->have_place,
	                             t->last_place.x, t->last_place.y,
	                             self->cascade_step, &x, &y);
	t->last_place = (struct wlr_box){ x, y, width, height };
	t->have_place = TRUE;

	c->geom = (struct wlr_box){ x, y, width, height };
	/* Tell on_client_map's tail this window is placed: without it the
	 * "centre a floating window at 0,0" step would undo the cascade. */
	c->pending_rule_geom_set = TRUE;

	release_maximized(self, m, c->tags, c);
}

/*
 * on_pre_arrange:
 * @comp: the compositor
 * @m: the output about to be arranged
 * @user_data: the module
 *
 * Mark first, float second: every window to float is marked in a first
 * pass and only then floated, so floating one cannot change how the
 * next is classified (a tabbed group's members were otherwise skipped as
 * "already floating" once their leader moved).
 */
static void
on_pre_arrange(
	GowlCompositor *comp,
	GowlMonitor    *m,
	gpointer        user_data
){
	GowlModuleFloatdesk *self = user_data;
	g_autoptr(GPtrArray) to_float = NULL;
	g_autoptr(GPtrArray) to_unfloat = NULL;
	GList               *l;
	guint                i;

	(void)comp;
	if (!floatdesk_active(self) || m == NULL)
		return;

	sync_layout(self, m);

	to_float = g_ptr_array_new();
	to_unfloat = g_ptr_array_new();
	for (l = self->compositor->clients; l != NULL; l = l->next) {
		GowlClient             *c = l->data;
		GowlFloatdeskClientView view;

		if (c->mon != m || c->scene == NULL)
			continue;
		client_view(c, &view);
		switch (gowl_floatdesk_state_decide(self->state, &view)) {
		case GOWL_FLOATDESK_ACTION_FLOAT:
			c->mode_floated = TRUE;
			g_ptr_array_add(to_float, c);
			break;
		case GOWL_FLOATDESK_ACTION_UNFLOAT:
			c->mode_floated = FALSE;
			g_ptr_array_add(to_unfloat, c);
			break;
		case GOWL_FLOATDESK_ACTION_CLEAR:
			c->mode_floated = FALSE;
			break;
		case GOWL_FLOATDESK_ACTION_NONE:
		default:
			break;
		}
		/* A window that has a workspace now has been placed by
		 * whatever placed it, mode or not. */
		if (c->tags != 0 && !floatdesk_client_on_floating(self, c))
			g_object_set_data(G_OBJECT(c), FLOATDESK_FRESH_KEY, NULL);
	}

	for (i = 0; i < to_float->len; i++) {
		GowlClient *c = g_ptr_array_index(to_float, i);

		gowl_compositor_float_in_place(self->compositor, c, TRUE);
		if (g_object_get_data(G_OBJECT(c), FLOATDESK_FRESH_KEY) != NULL)
			place_fresh(self, m, c);
	}
	for (i = 0; i < to_unfloat->len; i++) {
		GowlClient *c = g_ptr_array_index(to_unfloat, i);

		/* Back into the layout: a maximized or snapped size means
		 * nothing to a tile. */
		g_object_set_data(G_OBJECT(c), FLOATDESK_RESTORE_KEY, NULL);
		gowl_compositor_float_in_place(self->compositor, c, FALSE);
	}

	/* Title bars follow floating-on-a-floating-workspace, which the
	 * passes above may just have changed.  A window whose bar appears
	 * or goes is re-placed at the same frame, so its content moves. */
	for (l = self->compositor->clients; l != NULL; l = l->next) {
		GowlClient *c = l->data;
		gint        want;

		if (c->mon != m || c->scene == NULL)
			continue;
		g_object_set_data(G_OBJECT(c), FLOATDESK_FRESH_KEY, NULL);
		want = floatdesk_titlebar_wanted(self, c)
			? self->titlebar_height : 0;
		if (c->deco_top != want) {
			c->deco_top = want;
			if (c->isfloating && !c->isfullscreen)
				gowl_compositor_place_client(self->compositor, c,
					c->geom.x, c->geom.y,
					c->geom.width, c->geom.height);
			else if (want == 0)
				floatdesk_titlebar_drop(self, c);
		}
	}
	for (i = 0; i < to_float->len; i++) {
		GowlClient *c = g_ptr_array_index(to_float, i);

		/* Floated with no bar change: still has to be put at its
		 * geometry once as a floating window. */
		if (c->deco_top == 0)
			gowl_compositor_place_client(self->compositor, c,
				c->geom.x, c->geom.y,
				c->geom.width, c->geom.height);
	}
}

static void
on_arranged(
	GowlCompositor *comp,
	GowlMonitor    *m,
	gpointer        user_data
){
	GowlModuleFloatdesk *self = user_data;

	(void)comp;
	if (!floatdesk_active(self))
		return;
	floatdesk_dock_refresh(self, m);
}

/*
 * on_client_pre_map:
 * @comp: the compositor
 * @client: a window about to be placed for the first time
 * @user_data: the module
 *
 * Only marks it as not yet placed.  Whether it floats is decided later,
 * in the pre-arrange that sees it on its workspace -- after every rule
 * has had its say, so a window a rule floated keeps the rule's size.
 */
static void
on_client_pre_map(
	GowlCompositor *comp,
	GObject        *client,
	gpointer        user_data
){
	(void)comp;
	(void)user_data;
	if (GOWL_IS_CLIENT(client))
		g_object_set_data(client, FLOATDESK_FRESH_KEY,
		                  GINT_TO_POINTER(1));
}

static void
on_client_added(
	GowlCompositor *comp,
	GObject        *client,
	gpointer        user_data
){
	GowlModuleFloatdesk *self = user_data;

	(void)comp;
	if (!floatdesk_active(self) || !GOWL_IS_CLIENT(client))
		return;
	floatdesk_dock_client_mapped(self, GOWL_CLIENT(client));
	floatdesk_dock_refresh(self, GOWL_CLIENT(client)->mon);
}

static void
on_client_removed(
	GowlCompositor *comp,
	GObject        *client,
	gpointer        user_data
){
	GowlModuleFloatdesk *self = user_data;

	(void)comp;
	if (!GOWL_IS_CLIENT(client))
		return;
	if (self->snap_client == GOWL_CLIENT(client))
		self->snap_client = NULL;
	if (self->last_click_client == GOWL_CLIENT(client))
		self->last_click_client = NULL;
	floatdesk_titlebar_drop(self, GOWL_CLIENT(client));
	if (floatdesk_active(self))
		floatdesk_dock_refresh_all(self);
}

static void
on_focus_changed(
	GowlCompositor *comp,
	GObject        *client,
	gpointer        user_data
){
	GowlModuleFloatdesk *self = user_data;
	GList               *l;

	(void)comp;
	(void)client;
	if (!floatdesk_active(self))
		return;
	/* Every bar on screen, not just the two that changed: the focus
	 * signal names the new window only. */
	for (l = self->compositor->clients; l != NULL; l = l->next) {
		GowlClient *c = l->data;

		if (c->deco_top > 0)
			floatdesk_titlebar_sync(self, c);
	}
	floatdesk_dock_refresh_all(self);
}

static void
on_title_changed(
	GowlCompositor *comp,
	GowlClient     *client,
	gpointer        user_data
){
	GowlModuleFloatdesk *self = user_data;

	(void)comp;
	if (floatdesk_active(self) && client != NULL && client->deco_top > 0)
		floatdesk_titlebar_sync(self, client);
}

static void
on_client_minimized(
	GowlCompositor *comp,
	GowlClient     *client,
	gboolean        minimized,
	gpointer        user_data
){
	GowlModuleFloatdesk *self = user_data;
	static gint64        seq = 0;

	(void)comp;
	if (minimized && client != NULL)
		g_object_set_data(G_OBJECT(client), FLOATDESK_MIN_SEQ_KEY,
		                  GINT_TO_POINTER((gint)(++seq & G_MAXINT)));
	if (floatdesk_active(self))
		floatdesk_dock_refresh_all(self);
}

static void
on_monitor_removed(
	GowlCompositor *comp,
	GowlMonitor    *m,
	gpointer        user_data
){
	GowlModuleFloatdesk *self = user_data;

	(void)comp;
	floatdesk_dock_monitor_gone(self, m);
	g_hash_table_remove(self->monitors, m);
	if (self->snap_monitor == m)
		self->snap_monitor = NULL;
}

/* ================================================================ */
/* The layout entry                                                  */
/* ================================================================ */

/*
 * desktop_arrange:
 * @comp: the compositor
 * @m: the output
 *
 * The `desktop' layout.  By the time it runs, the pre-arrange has
 * floated every window on the view, so there is nothing to tile; a
 * window that is somehow still tiled (the module switched off between
 * the two) is shown where it is rather than left invisible.
 */
static void
desktop_arrange(
	GowlCompositor *comp,
	GowlMonitor    *m
){
	GList *tiled, *l;

	tiled = gowl_compositor_tiling_clients(comp, m);
	for (l = tiled; l != NULL; l = l->next) {
		GowlClient *c = l->data;

		gowl_compositor_place_client(comp, c, c->geom.x, c->geom.y,
		                             c->geom.width, c->geom.height);
	}
	g_list_free(tiled);
}

/* ================================================================ */
/* Minimize                                                          */
/* ================================================================ */

gchar *
floatdesk_minimize(
	GowlModuleFloatdesk *self,
	GowlClient          *c
){
	if (c == NULL)
		return g_strdup("ERROR no window");
	/* The key does nothing on a tiling workspace: minimizing a tile is
	 * a window disappearing from a layout for no visible reason. */
	if (!floatdesk_client_on_floating(self, c))
		return g_strdup("ERROR the window is not on a floating desktop");
	if (!gowl_compositor_minimize_client(self->compositor, c))
		return g_strdup_printf("ERROR client %u cannot be minimized",
		                       c->id);
	return g_strdup_printf("OK minimized %u", c->id);
}

void
floatdesk_restore(
	GowlModuleFloatdesk *self,
	GowlClient          *c
){
	if (c == NULL || !c->isminimized)
		return;
	gowl_compositor_unminimize_client(self->compositor, c, NULL, 0);
}

/*
 * last_minimized:
 * @self: the module
 *
 * Returns: (transfer none) (nullable): the window minimized most
 *   recently
 */
static GowlClient *
last_minimized(GowlModuleFloatdesk *self)
{
	GowlClient *best = NULL;
	gint        best_seq = -1;
	GList      *l;

	for (l = self->compositor->clients; l != NULL; l = l->next) {
		GowlClient *c = l->data;
		gint        seq;

		if (!c->isminimized)
			continue;
		seq = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(c),
		                                        FLOATDESK_MIN_SEQ_KEY));
		if (seq > best_seq) {
			best_seq = seq;
			best = c;
		}
	}
	return best;
}

/* ================================================================ */
/* Dock settings: the run-time value, else the config                */
/* ================================================================ */

const gchar *
floatdesk_dock_position(GowlModuleFloatdesk *self)
{
	const gchar *p = gowl_floatdesk_state_get_dock_position(self->state);

	if (p != NULL)
		return p;
	return self->config_dock_position != NULL
		? self->config_dock_position : "bottom";
}

gboolean
floatdesk_dock_autohide(GowlModuleFloatdesk *self)
{
	gint a = gowl_floatdesk_state_get_dock_autohide(self->state);

	return a >= 0 ? a == 1 : self->config_dock_autohide;
}

gchar **
floatdesk_dock_pins(GowlModuleFloatdesk *self)
{
	const gchar *const *pins;

	pins = gowl_floatdesk_state_get_dock_pins(self->state);
	if (pins != NULL)
		return g_strdupv((gchar **)pins);
	if (self->config_pins != NULL)
		return g_strdupv(self->config_pins);
	return g_new0(gchar *, 1);
}

/* ================================================================ */
/* Commands                                                          */
/* ================================================================ */

/*
 * parse_tag_arg:
 * @self: the module
 * @args: (nullable): "" for the selected view, else a tag number 1-32
 * @mask: (out): the tags meant
 * @error_out: (out): an error reply
 *
 * Returns: %TRUE on success
 */
static gboolean
parse_tag_arg(
	GowlModuleFloatdesk *self,
	const gchar         *args,
	guint32             *mask,
	gchar              **error_out
){
	g_autofree gchar *copy = NULL;
	gchar            *end;
	gint64            tag;
	GowlMonitor      *m;

	*error_out = NULL;
	copy = g_strstrip(g_strdup(args != NULL ? args : ""));
	if (*copy == '\0') {
		m = self->compositor->selmon;
		*mask = m != NULL ? m->tagset[m->seltags] : 0;
		if (*mask == 0) {
			*error_out = g_strdup("ERROR no workspace in view");
			return FALSE;
		}
		return TRUE;
	}
	tag = g_ascii_strtoll(copy, &end, 10);
	if (end == copy || *end != '\0' || tag < 1
	    || tag > GOWL_FLOATDESK_MAX_TAGS) {
		*error_out = g_strdup_printf("ERROR \"%s\" is not a tag from 1 "
		                             "to %d", copy,
		                             GOWL_FLOATDESK_MAX_TAGS);
		return FALSE;
	}
	*mask = (guint32)1u << (tag - 1);
	return TRUE;
}

/*
 * arrange_all:
 * @self: the module
 *
 * Re-arranges every output, so a change to the mode reaches windows on
 * workspaces that are not in view as well.
 */
static void
arrange_all(GowlModuleFloatdesk *self)
{
	GList *l;

	for (l = self->compositor->monitors; l != NULL; l = l->next)
		gowl_compositor_arrange(self->compositor, l->data);
}

/*
 * set_mode:
 * @self: the module
 * @mask: the tags
 * @floating: float them or tile them
 *
 * Sets the mode, saves it, and applies it -- through the layout when the
 * tags are the view, so the layout cycle and the toggle are the same
 * transition.  The reply says what is now TRUE: a layout that could not
 * be set puts the state back and is reported as an error, never as a
 * mode that is not on screen.
 */
static gchar *
set_mode(
	GowlModuleFloatdesk *self,
	guint32              mask,
	gboolean             floating
){
	GowlMonitor      *m;
	GowlLayoutEntry  *cur;
	g_autofree gchar *tags = NULL;
	gboolean          in_view, saved;
	gint8             before[GOWL_FLOATDESK_MAX_TAGS];
	guint             tag;

	m = self->compositor->selmon;
	in_view = m != NULL && m->tagset[m->seltags] == mask;
	tags = mask_text(mask);

	if (floating && gowl_layout_lookup(self->compositor,
	                                   GOWL_FLOATDESK_LAYOUT) == NULL)
		return g_strdup("ERROR the desktop layout is not registered");

	for (tag = 0; tag < GOWL_FLOATDESK_MAX_TAGS; tag++)
		before[tag] = (gint8)gowl_floatdesk_state_get_mode(self->state,
		                                                   tag);

	if (floating && m != NULL) {
		cur = gowl_layout_get(self->compositor, m);
		if (in_view && cur != NULL)
			remember_previous(self, mask, cur->name);
	}
	gowl_floatdesk_state_set_mask(self->state, mask, floating);

	if (in_view) {
		const gchar *target = floating ? GOWL_FLOATDESK_LAYOUT
		                               : leave_layout_for(self, mask);

		if (target == NULL || !gowl_layout_set(self->compositor, m,
		                                       target)) {
			for (tag = 0; tag < GOWL_FLOATDESK_MAX_TAGS; tag++)
				gowl_floatdesk_state_set_mode(self->state, tag,
					(GowlFloatdeskMode)before[tag]);
			return g_strdup_printf("ERROR could not set the %s "
			                       "layout on tag %s",
			                       target != NULL ? target : "tiling",
			                       tags);
		}
	}
	/* Every output: the same tag may be in view on another one, and
	 * windows on it anywhere have to follow. */
	arrange_all(self);

	saved = floatdesk_save(self);
	/* The layout change already put "Floating desktop" / the tiling
	 * layout's name in the toast; a tag out of view gets its own. */
	if (!in_view) {
		g_autofree gchar *msg = g_strdup_printf(
			"Tag %s: %s", tags,
			floating ? "floating desktop" : "tiling");
		floatdesk_toast(self, m, msg);
	}
	floatdesk_event(self, "mode %s %s", tags,
	                floating ? "floating" : "tiled");
	return g_strdup_printf("OK tag %s %s%s", tags,
	                       floating ? "floating" : "tiled",
	                       saved ? "" : " (not saved)");
}

static gchar *
cmd_status(GowlModuleFloatdesk *self)
{
	GowlMonitor      *m = self->compositor->selmon;
	g_autofree gchar *floating = NULL;
	g_autofree gchar *tiled = NULL;
	g_autofree gchar *view = NULL;
	g_auto(GStrv)     pins = NULL;
	g_autofree gchar *pin_text = NULL;
	GString          *minimized;
	guint32           fmask = 0, tmask = 0;
	guint             tag;
	GList            *l;

	for (tag = 0; tag < GOWL_FLOATDESK_MAX_TAGS; tag++) {
		GowlFloatdeskMode mode;

		mode = gowl_floatdesk_state_get_mode(self->state, tag);
		if (mode == GOWL_FLOATDESK_MODE_FLOATING)
			fmask |= (guint32)1u << tag;
		else if (mode == GOWL_FLOATDESK_MODE_TILED)
			tmask |= (guint32)1u << tag;
	}
	floating = mask_text(fmask);
	tiled = mask_text(tmask);
	view = mask_text(m != NULL ? m->tagset[m->seltags] : 0);

	minimized = g_string_new(NULL);
	for (l = self->compositor->clients; l != NULL; l = l->next) {
		GowlClient *c = l->data;

		if (!c->isminimized)
			continue;
		if (minimized->len > 0)
			g_string_append_c(minimized, ',');
		g_string_append_printf(minimized, "%u", c->id);
	}
	pins = floatdesk_dock_pins(self);
	pin_text = g_strjoinv(",", pins);

	return g_strdup_printf("OK view=%s view-floating=%d default=%d "
	                       "floating-tags=%s tiled-tags=%s minimized=%s "
	                       "dock=%s autohide=%d pins=%s state=%s",
	                       view, floatdesk_view_floating(self, m) ? 1 : 0,
	                       gowl_floatdesk_state_get_default_floating(
	                               self->state) ? 1 : 0,
	                       floating, tiled,
	                       (gchar *)g_string_free(minimized, FALSE),
	                       floatdesk_dock_position(self),
	                       floatdesk_dock_autohide(self) ? 1 : 0,
	                       pin_text,
	                       self->state_path != NULL ? self->state_path : "");
}

/*
 * parse_switch:
 * @text: "on", "off" or "toggle" (and the usual synonyms)
 * @current: the value now
 * @out: (out): the value asked for
 *
 * Returns: %TRUE if @text was understood
 */
static gboolean
parse_switch(
	const gchar *text,
	gboolean     current,
	gboolean    *out
){
	g_autofree gchar *t = g_strstrip(g_strdup(text != NULL ? text : ""));

	if (*t == '\0' || g_ascii_strcasecmp(t, "toggle") == 0) {
		*out = !current;
		return TRUE;
	}
	if (g_ascii_strcasecmp(t, "on") == 0 || g_ascii_strcasecmp(t, "true") == 0
	    || g_strcmp0(t, "1") == 0) {
		*out = TRUE;
		return TRUE;
	}
	if (g_ascii_strcasecmp(t, "off") == 0 || g_ascii_strcasecmp(t, "false") == 0
	    || g_strcmp0(t, "0") == 0) {
		*out = FALSE;
		return TRUE;
	}
	return FALSE;
}

static gchar *
cmd_default(
	GowlModuleFloatdesk *self,
	const gchar         *args
){
	gboolean want;
	gboolean saved;

	if (!parse_switch(args, gowl_floatdesk_state_get_default_floating(
	                          self->state), &want))
		return g_strdup("ERROR floatdesk-default takes on, off or "
		                "toggle");
	gowl_floatdesk_state_set_default_floating(self->state, want);
	arrange_all(self);
	saved = floatdesk_save(self);
	floatdesk_toast(self, NULL, want
		? "Every workspace: floating desktop"
		: "Every workspace: tiling");
	floatdesk_event(self, "default %s", want ? "floating" : "tiled");
	return g_strdup_printf("OK default %s%s", want ? "floating" : "tiled",
	                       saved ? "" : " (not saved)");
}

static gchar *
cmd_snap(
	GowlModuleFloatdesk *self,
	const gchar         *args
){
	g_auto(GStrv)       words = NULL;
	g_autofree gchar   *copy = NULL;
	GowlFloatdeskSnap   kind;
	GowlClient         *c;
	gchar              *err = NULL;

	/* The arguments are the caller's (a literal, often): strip a copy. */
	copy = g_strstrip(g_strdup(args != NULL ? args : ""));
	words = g_strsplit(copy, " ", 2);
	if (words[0] == NULL)
		return g_strdup("ERROR floatdesk-snap takes left, right or max");
	if (g_strcmp0(words[0], "left") == 0)
		kind = GOWL_FLOATDESK_SNAP_LEFT;
	else if (g_strcmp0(words[0], "right") == 0)
		kind = GOWL_FLOATDESK_SNAP_RIGHT;
	else if (g_strcmp0(words[0], "max") == 0
	         || g_strcmp0(words[0], "maximize") == 0)
		kind = GOWL_FLOATDESK_SNAP_MAXIMIZE;
	else
		return g_strdup_printf("ERROR unknown snap \"%s\"; left, right "
		                       "or max", words[0]);
	c = find_client(self, words[1], &err);
	if (c == NULL)
		return err;
	if (!floatdesk_client_on_floating(self, c) || !c->isfloating)
		return g_strdup("ERROR the window is not floating on a floating "
		                "desktop");
	floatdesk_snap(self, c, kind);
	return g_strdup_printf("OK snapped %u %s", c->id, words[0]);
}

static gchar *
cmd_dock(
	GowlModuleFloatdesk *self,
	const gchar         *verb,
	const gchar         *args
){
	g_autofree gchar *arg = g_strstrip(g_strdup(args != NULL ? args : ""));

	if (g_strcmp0(verb, "position") == 0) {
		if (!gowl_floatdesk_state_set_dock_position(self->state, arg))
			return g_strdup_printf("ERROR dock position \"%s\": bottom, "
			                       "left, right or top", arg);
		floatdesk_dock_refresh_all(self);
		floatdesk_event(self, "dock position %s", arg);
		return g_strdup_printf("OK dock %s%s", arg,
		                       floatdesk_save(self) ? "" : " (not saved)");
	}
	if (g_strcmp0(verb, "autohide") == 0) {
		gboolean want;

		if (!parse_switch(arg, floatdesk_dock_autohide(self), &want))
			return g_strdup("ERROR floatdesk-dock-autohide takes on, "
			                "off or toggle");
		gowl_floatdesk_state_set_dock_autohide(self->state, want ? 1 : 0);
		floatdesk_dock_refresh_all(self);
		floatdesk_event(self, "dock autohide %s", want ? "on" : "off");
		return g_strdup_printf("OK autohide %s%s", want ? "on" : "off",
		                       floatdesk_save(self) ? "" : " (not saved)");
	}
	if (g_strcmp0(verb, "pin") == 0) {
		g_auto(GStrv) words = g_strsplit(arg, " ", 2);
		gint          index = -1;

		if (words[0] == NULL || *words[0] == '\0')
			return g_strdup("ERROR floatdesk-dock-pin takes an app id");
		if (words[1] != NULL)
			index = (gint)g_ascii_strtoll(words[1], NULL, 10);
		if (!floatdesk_dock_pin(self, words[0], index))
			return g_strdup_printf("OK %s already pinned", words[0]);
		return g_strdup_printf("OK pinned %s%s", words[0],
		                       floatdesk_save(self) ? "" : " (not saved)");
	}
	if (g_strcmp0(verb, "unpin") == 0) {
		if (*arg == '\0')
			return g_strdup("ERROR floatdesk-dock-unpin takes an app id");
		if (!floatdesk_dock_unpin(self, arg))
			return g_strdup_printf("ERROR %s is not pinned", arg);
		return g_strdup_printf("OK unpinned %s%s", arg,
		                       floatdesk_save(self) ? "" : " (not saved)");
	}
	if (g_strcmp0(verb, "launch") == 0)
		return floatdesk_dock_launch(self, arg);
	if (g_strcmp0(verb, "activate") == 0)
		return floatdesk_dock_activate(self, arg);
	if (g_strcmp0(verb, "apps") == 0) {
		floatdesk_dock_toggle_grid(self);
		return g_strdup("OK");
	}
	if (g_strcmp0(verb, "status") == 0)
		return floatdesk_dock_status(self);
	return NULL;
}

static gchar *
floatdesk_handle_command(
	GowlIpcHandler *handler,
	const gchar    *command,
	const gchar    *args
){
	GowlModuleFloatdesk *self;
	const gchar         *verb;
	guint32              mask;
	gchar               *err = NULL;
	GowlClient          *c;

	if (command == NULL || !g_str_has_prefix(command, FLOATDESK_PREFIX))
		return NULL;
	self = GOWL_MODULE_FLOATDESK(handler);
	if (!floatdesk_active(self))
		return g_strdup("ERROR the floating desktop has not started");
	verb = command + strlen(FLOATDESK_PREFIX);

	if (g_strcmp0(verb, "status") == 0)
		return cmd_status(self);
	if (g_strcmp0(verb, "toggle") == 0 || g_strcmp0(verb, "on") == 0
	    || g_strcmp0(verb, "off") == 0) {
		gboolean want;

		if (!parse_tag_arg(self, args, &mask, &err))
			return err;
		if (g_strcmp0(verb, "toggle") == 0)
			want = !gowl_floatdesk_state_mask_floating(self->state, mask);
		else
			want = g_strcmp0(verb, "on") == 0;
		return set_mode(self, mask, want);
	}
	if (g_strcmp0(verb, "default") == 0)
		return cmd_default(self, args);
	if (g_strcmp0(verb, "minimize") == 0) {
		c = find_client(self, args, &err);
		return c != NULL ? floatdesk_minimize(self, c) : err;
	}
	if (g_strcmp0(verb, "restore") == 0) {
		g_autofree gchar *a = g_strstrip(g_strdup(args != NULL ? args : ""));

		if (*a == '\0') {
			c = last_minimized(self);
			if (c == NULL)
				return g_strdup("ERROR nothing is minimized");
		} else {
			c = find_client(self, a, &err);
			if (c == NULL)
				return err;
			if (!c->isminimized)
				return g_strdup_printf("ERROR client %u is not "
				                       "minimized", c->id);
		}
		floatdesk_restore(self, c);
		return g_strdup_printf("OK restored %u", c->id);
	}
	if (g_strcmp0(verb, "maximize") == 0) {
		c = find_client(self, args, &err);
		if (c == NULL)
			return err;
		if (!floatdesk_client_on_floating(self, c) || !c->isfloating)
			return g_strdup("ERROR the window is not floating on a "
			                "floating desktop");
		floatdesk_maximize_toggle(self, c);
		return g_strdup_printf("OK %s %u", floatdesk_is_maximized(c)
		                       ? "maximized" : "restored", c->id);
	}
	if (g_strcmp0(verb, "snap") == 0)
		return cmd_snap(self, args);
	if (g_str_has_prefix(verb, "dock-")) {
		gchar *reply = cmd_dock(self, verb + strlen("dock-"), args);

		if (reply != NULL)
			return reply;
	}
	return g_strdup_printf("ERROR unknown command %s; see floatdesk-toggle, "
	                       "-on, -off, -default, -status, -minimize, "
	                       "-restore, -maximize, -snap and -dock-*",
	                       command);
}

static void
floatdesk_ipc_init(GowlIpcHandlerInterface *iface)
{
	iface->handle_command = floatdesk_handle_command;
}

/* ================================================================ */
/* Pointer and keys                                                  */
/* ================================================================ */

static gboolean
floatdesk_handle_button(
	GowlMouseHandler *handler,
	guint             button,
	guint             state,
	guint             modifiers
){
	GowlModuleFloatdesk *self = GOWL_MODULE_FLOATDESK(handler);

	(void)modifiers;
	if (!floatdesk_active(self) || self->compositor->locked)
		return FALSE;
	/* The dock and its menus sit above every window: they are asked
	 * first. */
	if (floatdesk_dock_button(self, button, state != 0))
		return TRUE;
	if (state == 0) {
		floatdesk_titlebar_release(self, button);
		return FALSE;
	}
	return floatdesk_titlebar_button(self, button, TRUE);
}

static gboolean
floatdesk_handle_motion(
	GowlMouseHandler *handler,
	gdouble           x,
	gdouble           y
){
	GowlModuleFloatdesk *self = GOWL_MODULE_FLOATDESK(handler);

	if (!floatdesk_active(self) || self->compositor->locked)
		return FALSE;
	return floatdesk_dock_motion(self, x, y);
}

/* The wheel scrolls the apps grid while the pointer is over it. */
static gboolean
floatdesk_handle_axis(
	GowlMouseHandler *handler,
	guint             axis,
	gdouble           delta,
	gint              discrete,
	guint             modifiers
){
	GowlModuleFloatdesk *self = GOWL_MODULE_FLOATDESK(handler);

	(void)axis;
	(void)discrete;
	(void)modifiers;
	if (!floatdesk_active(self) || self->compositor->locked)
		return FALSE;
	return floatdesk_dock_axis(self, delta);
}

static void
floatdesk_mouse_init(GowlMouseHandlerInterface *iface)
{
	iface->handle_button = floatdesk_handle_button;
	iface->handle_motion = floatdesk_handle_motion;
	iface->handle_axis = floatdesk_handle_axis;
}

static gboolean
floatdesk_handle_key(
	GowlKeybindHandler *handler,
	guint               modifiers,
	guint               keysym,
	gboolean            pressed
){
	GowlModuleFloatdesk *self = GOWL_MODULE_FLOATDESK(handler);

	(void)modifiers;
	if (!floatdesk_active(self))
		return FALSE;
	return floatdesk_dock_key(self, keysym, pressed);
}

static void
floatdesk_keybind_init(GowlKeybindHandlerInterface *iface)
{
	iface->handle_key = floatdesk_handle_key;
}

/*
 * floatdesk_focus_on_hover:
 *
 * Focus follows the click on a floating desktop.  Overlapping windows
 * make sloppy focus a hazard there: crossing one window on the way to
 * another hands the keyboard to the one crossed.
 */
static gboolean
floatdesk_focus_on_hover(
	GowlFocusPolicy *policy,
	gpointer         client
){
	GowlModuleFloatdesk *self = GOWL_MODULE_FLOATDESK(policy);
	GowlClient          *c = client;

	if (!floatdesk_active(self) || !self->focus_follows_click
	    || c == NULL || !GOWL_IS_CLIENT(c))
		return TRUE;
	return !floatdesk_client_on_floating(self, c);
}

static gboolean
floatdesk_focus_on_map(
	GowlFocusPolicy *policy,
	gpointer         client
){
	(void)policy;
	(void)client;
	return TRUE;
}

static void
floatdesk_focus_init(GowlFocusPolicyInterface *iface)
{
	iface->focus_on_hover = floatdesk_focus_on_hover;
	iface->focus_on_map = floatdesk_focus_on_map;
}

/*
 * floatdesk_client_placed:
 *
 * Every drawn frame of every window comes through here -- the module's
 * only scene-effect hook.  The title bar follows the frame: stretched
 * while an animation is moving it, redrawn at the size it lands on.
 */
static void
floatdesk_client_placed(
	GowlSceneEffect *effect,
	GowlCompositor  *comp,
	GowlClient      *c,
	gboolean         settled
){
	GowlModuleFloatdesk *self = GOWL_MODULE_FLOATDESK(effect);

	(void)comp;
	(void)settled;
	if (!floatdesk_active(self) || c == NULL)
		return;
	if (c->deco_top > 0)
		floatdesk_titlebar_sync(self, c);
}

static void
floatdesk_effect_init(GowlSceneEffectInterface *iface)
{
	iface->client_placed = floatdesk_client_placed;
}

/* ================================================================ */
/* Settings                                                          */
/* ================================================================ */

/*
 * setting_double:
 * @settings: the module's settings
 * @key: the key
 * @lo: the lowest accepted value
 * @hi: the highest accepted value
 * @out: (inout): set when the value is valid, else kept
 */
static void
setting_double(
	GHashTable  *settings,
	const gchar *key,
	gdouble      lo,
	gdouble      hi,
	gdouble     *out
){
	const gchar *text = g_hash_table_lookup(settings, key);
	gchar       *end = NULL;
	gdouble      v;

	if (text == NULL)
		return;
	v = g_ascii_strtod(text, &end);
	if (end == text || *end != '\0' || !(v >= lo && v <= hi)) {
		g_warning("floatdesk: %s must be from %g to %g, not \"%s\"; "
		          "keeping %g", key, lo, hi, text, *out);
		return;
	}
	*out = v;
}

static void
setting_int(
	GHashTable  *settings,
	const gchar *key,
	gint         lo,
	gint         hi,
	gint        *out
){
	const gchar *text = g_hash_table_lookup(settings, key);
	gchar       *end = NULL;
	gint64       v;

	if (text == NULL)
		return;
	v = g_ascii_strtoll(text, &end, 10);
	if (end == text || *end != '\0' || v < lo || v > hi) {
		g_warning("floatdesk: %s must be from %d to %d, not \"%s\"; "
		          "keeping %d", key, lo, hi, text, *out);
		return;
	}
	*out = (gint)v;
}

static gboolean
setting_bool(
	GHashTable  *settings,
	const gchar *key,
	gboolean    *out
){
	const gchar *text = g_hash_table_lookup(settings, key);

	if (text == NULL)
		return FALSE;
	if (g_ascii_strcasecmp(text, "true") == 0
	    || g_ascii_strcasecmp(text, "yes") == 0
	    || g_ascii_strcasecmp(text, "on") == 0) {
		*out = TRUE;
		return TRUE;
	}
	if (g_ascii_strcasecmp(text, "false") == 0
	    || g_ascii_strcasecmp(text, "no") == 0
	    || g_ascii_strcasecmp(text, "off") == 0) {
		*out = FALSE;
		return TRUE;
	}
	g_warning("floatdesk: %s must be true or false, not \"%s\"", key, text);
	return FALSE;
}

/*
 * floatdesk_configure:
 * @mod: the module
 * @config: the `modules: floatdesk:' settings, a #GHashTable of strings
 *
 * Every key is optional and a bad value is refused with a warning, the
 * old one kept.  `pins' arrives as one string per line (a YAML list).
 */
static void
floatdesk_configure(
	GowlModule *mod,
	gpointer    config
){
	GowlModuleFloatdesk *self = GOWL_MODULE_FLOATDESK(mod);
	GHashTable          *s = config;
	const gchar         *text;
	gboolean             flag;

	if (s == NULL)
		return;

	if (setting_bool(s, "default-floating", &flag)) {
		self->config_default_set = TRUE;
		self->config_default_floating = flag;
	}
	setting_double(s, "width-pct", 0.1, 1.0, &self->width_pct);
	setting_double(s, "height-pct", 0.1, 1.0, &self->height_pct);
	setting_int(s, "cascade", 0, 512, &self->cascade_step);
	setting_bool(s, "focus-follows-click", &self->focus_follows_click);
	setting_bool(s, "titlebars", &self->titlebars);
	setting_int(s, "titlebar-height", 16, 96, &self->titlebar_height);
	setting_int(s, "snap-threshold", 1, 64, &self->snap_threshold);
	setting_bool(s, "dock", &self->dock_enabled);
	setting_int(s, "dock-icon-size", 16, 128, &self->dock_icon_size);
	setting_bool(s, "dock-autohide", &self->config_dock_autohide);

	text = g_hash_table_lookup(s, "dock-position");
	if (text != NULL) {
		if (g_strcmp0(text, "bottom") == 0 || g_strcmp0(text, "top") == 0
		    || g_strcmp0(text, "left") == 0
		    || g_strcmp0(text, "right") == 0) {
			g_free(self->config_dock_position);
			self->config_dock_position = g_strdup(text);
		} else {
			g_warning("floatdesk: dock-position must be bottom, left, "
			          "right or top, not \"%s\"", text);
		}
	}
	text = g_hash_table_lookup(s, "pins");
	if (text != NULL) {
		g_auto(GStrv) lines = g_strsplit_set(text, "\n,", -1);
		GPtrArray    *pins = g_ptr_array_new();
		guint         i;

		for (i = 0; lines[i] != NULL; i++) {
			g_strstrip(lines[i]);
			if (*lines[i] != '\0')
				g_ptr_array_add(pins, g_strdup(lines[i]));
		}
		g_ptr_array_add(pins, NULL);
		g_strfreev(self->config_pins);
		self->config_pins = (gchar **)g_ptr_array_free(pins, FALSE);
	}
	text = g_hash_table_lookup(s, "terminal");
	if (text != NULL && *text != '\0') {
		g_free(self->terminal);
		self->terminal = g_strdup(text);
	}
	text = g_hash_table_lookup(s, "state-file");
	if (text != NULL && *text != '\0') {
		g_free(self->state_path);
		self->state_path = g_strdup(text);
	}

	if (self->config_default_set)
		gowl_floatdesk_state_set_default_floating(self->state,
			self->config_default_floating);

	if (floatdesk_active(self)) {
		arrange_all(self);
		floatdesk_dock_refresh_all(self);
	}
}

/* ================================================================ */
/* Lifetime                                                          */
/* ================================================================ */

/*
 * load_state:
 * @self: the module
 *
 * A state file that cannot be read is reported and left alone -- not
 * overwritten by the empty state the next save would write.
 */
static void
load_state(GowlModuleFloatdesk *self)
{
	GError *error = NULL;

	if (self->state_path == NULL)
		self->state_path = gowl_floatdesk_state_default_path();
	if (!gowl_floatdesk_state_load(self->state, self->state_path, &error)) {
		g_warning("floatdesk: %s; starting with every workspace tiled "
		          "and saving nothing until a change is made",
		          error->message);
		{
			g_autofree gchar *msg = g_strdup_printf(
				"Floating desktop: could not read state (%s)",
				error->message);
			floatdesk_toast(self, NULL, msg);
		}
		g_error_free(error);
	}
	/* The config's default, when it gives one, outranks the saved one. */
	if (self->config_default_set)
		gowl_floatdesk_state_set_default_floating(self->state,
			self->config_default_floating);
}

/*
 * detach:
 * @self: the module
 * @give_back: tile again every window the mode floated
 *
 * Disconnects from the compositor.  Switching the module off gives the
 * windows back -- nothing is left floating that only the mode wanted
 * floating -- and takes the title bars and docks away.
 */
static void
detach(
	GowlModuleFloatdesk *self,
	gboolean             give_back
){
	GowlCompositor *comp = self->compositor;
	GList          *l;

	if (comp == NULL)
		return;

	if (self->snap_idle != NULL) {
		wl_event_source_remove(self->snap_idle);
		self->snap_idle = NULL;
	}
	if (self->dock_timer != NULL) {
		wl_event_source_remove(self->dock_timer);
		self->dock_timer = NULL;
	}
	floatdesk_dock_destroy_all(self);

	if (self->pre_arrange_id != 0)
		g_signal_handler_disconnect(comp, self->pre_arrange_id);
	if (self->arranged_id != 0)
		g_signal_handler_disconnect(comp, self->arranged_id);
	if (self->pre_map_id != 0)
		g_signal_handler_disconnect(comp, self->pre_map_id);
	if (self->added_id != 0)
		g_signal_handler_disconnect(comp, self->added_id);
	if (self->removed_id != 0)
		g_signal_handler_disconnect(comp, self->removed_id);
	if (self->focus_id != 0)
		g_signal_handler_disconnect(comp, self->focus_id);
	if (self->title_id != 0)
		g_signal_handler_disconnect(comp, self->title_id);
	if (self->minimized_id != 0)
		g_signal_handler_disconnect(comp, self->minimized_id);
	if (self->monitor_removed_id != 0)
		g_signal_handler_disconnect(comp, self->monitor_removed_id);
	self->pre_arrange_id = self->arranged_id = self->pre_map_id = 0;
	self->added_id = self->removed_id = self->focus_id = 0;
	self->title_id = self->minimized_id = self->monitor_removed_id = 0;

	for (l = comp->clients; l != NULL; l = l->next) {
		GowlClient *c = l->data;

		g_object_set_data(G_OBJECT(c), FLOATDESK_FRESH_KEY, NULL);
		g_object_set_data(G_OBJECT(c), FLOATDESK_RESTORE_KEY, NULL);
		if (c->deco_top > 0 && c->scene != NULL) {
			c->deco_top = 0;
			if (c->isfloating && !c->isfullscreen)
				gowl_compositor_place_client(comp, c,
					c->geom.x, c->geom.y,
					c->geom.width, c->geom.height);
		}
		c->deco_top = 0;
		if (give_back && c->mode_floated) {
			c->mode_floated = FALSE;
			if (c->scene != NULL)
				gowl_compositor_float_in_place(comp, c, FALSE);
		}
	}
	floatdesk_titlebar_drop_all(self);

	if (comp->layouts != NULL)
		gowl_layout_unregister(comp, GOWL_FLOATDESK_LAYOUT);
	g_hash_table_remove_all(self->monitors);
	g_object_remove_weak_pointer(G_OBJECT(comp),
	                             (gpointer *)&self->compositor);
	self->compositor = NULL;

	if (give_back) {
		for (l = comp->monitors; l != NULL; l = l->next)
			gowl_compositor_arrange(comp, l->data);
	}
}

static void
floatdesk_on_startup(
	GowlStartupHandler *handler,
	gpointer            compositor
){
	GowlModuleFloatdesk *self = GOWL_MODULE_FLOATDESK(handler);
	GowlCompositor      *comp = compositor;
	GList               *l;

	if (comp == NULL || self->compositor == comp)
		return;
	detach(self, FALSE);
	self->compositor = comp;
	g_object_add_weak_pointer(G_OBJECT(comp), (gpointer *)&self->compositor);

	load_state(self);
	if (self->theme != NULL)
		gowl_bar_theme_free(self->theme);
	self->theme = gowl_bar_theme_new_for_palette(
		comp->config != NULL ? gowl_config_get_palette(comp->config)
		                     : NULL);

	/* After every other layout, so the cycle reads tile -> ... ->
	 * desktop.  The module is the entry's provider: switching it off
	 * takes the layout out of the cycle. */
	gowl_layout_register(comp, GOWL_FLOATDESK_LAYOUT, FLOATDESK_SYMBOL,
	                     desktop_arrange, self);

	self->pre_arrange_id = g_signal_connect(comp, "pre-arrange",
		G_CALLBACK(on_pre_arrange), self);
	self->arranged_id = g_signal_connect(comp, "arranged",
		G_CALLBACK(on_arranged), self);
	self->pre_map_id = g_signal_connect(comp, "client-pre-map",
		G_CALLBACK(on_client_pre_map), self);
	self->added_id = g_signal_connect(comp, "client-added",
		G_CALLBACK(on_client_added), self);
	self->removed_id = g_signal_connect(comp, "client-removed",
		G_CALLBACK(on_client_removed), self);
	self->focus_id = g_signal_connect(comp, "focus-changed",
		G_CALLBACK(on_focus_changed), self);
	self->title_id = g_signal_connect(comp, "client-title-changed",
		G_CALLBACK(on_title_changed), self);
	self->minimized_id = g_signal_connect(comp, "client-minimized",
		G_CALLBACK(on_client_minimized), self);
	self->monitor_removed_id = g_signal_connect(comp, "monitor-removed",
		G_CALLBACK(on_monitor_removed), self);

	/* Restore the saved modes onto every output now, not at the next
	 * key press. */
	for (l = comp->monitors; l != NULL; l = l->next)
		gowl_compositor_arrange(comp, l->data);
}

static void
floatdesk_startup_init(GowlStartupHandlerInterface *iface)
{
	iface->on_startup = floatdesk_on_startup;
}

static void
floatdesk_on_shutdown(
	GowlShutdownHandler *handler,
	gpointer             compositor
){
	GowlModuleFloatdesk *self = GOWL_MODULE_FLOATDESK(handler);

	(void)compositor;
	/* The compositor is going away with its windows: nothing to give
	 * back, only the hooks to take off while it is still alive. */
	detach(self, FALSE);
}

static void
floatdesk_shutdown_init(GowlShutdownHandlerInterface *iface)
{
	iface->on_shutdown = floatdesk_on_shutdown;
}

static gboolean
floatdesk_activate(GowlModule *mod)
{
	(void)mod;
	return TRUE;
}

static void
floatdesk_deactivate(GowlModule *mod)
{
	detach(GOWL_MODULE_FLOATDESK(mod), TRUE);
}

static const gchar *
floatdesk_get_name(GowlModule *mod)
{
	(void)mod;
	return "floatdesk";
}

static const gchar *
floatdesk_get_description(GowlModule *mod)
{
	(void)mod;
	return "Floating desktop per workspace: title bars, minimize, snap, "
	       "and a dock";
}

static const gchar *
floatdesk_get_version(GowlModule *mod)
{
	(void)mod;
	return "0.1.0";
}

static void
floatdesk_finalize(GObject *object)
{
	GowlModuleFloatdesk *self = GOWL_MODULE_FLOATDESK(object);

	detach(self, FALSE);
	g_clear_pointer(&self->state, gowl_floatdesk_state_free);
	g_clear_pointer(&self->state_path, g_free);
	g_clear_pointer(&self->monitors, g_hash_table_unref);
	g_clear_pointer(&self->titlebar_nodes, g_hash_table_unref);
	g_clear_pointer(&self->docks, g_hash_table_unref);
	g_clear_pointer(&self->launching, g_hash_table_unref);
	g_clear_pointer(&self->icons, g_hash_table_unref);
	g_clear_pointer(&self->catalog, gowl_floatdesk_catalog_free);
	g_clear_pointer(&self->theme, gowl_bar_theme_free);
	g_clear_pointer(&self->config_dock_position, g_free);
	g_clear_pointer(&self->config_pins, g_strfreev);
	g_clear_pointer(&self->terminal, g_free);
	G_OBJECT_CLASS(gowl_module_floatdesk_parent_class)->finalize(object);
}

static void
gowl_module_floatdesk_class_init(GowlModuleFloatdeskClass *klass)
{
	GowlModuleClass *mod_class = GOWL_MODULE_CLASS(klass);

	mod_class->activate        = floatdesk_activate;
	mod_class->deactivate      = floatdesk_deactivate;
	mod_class->get_name        = floatdesk_get_name;
	mod_class->get_description = floatdesk_get_description;
	mod_class->get_version     = floatdesk_get_version;
	mod_class->configure       = floatdesk_configure;
	G_OBJECT_CLASS(klass)->finalize = floatdesk_finalize;
}

static void
gowl_module_floatdesk_init(GowlModuleFloatdesk *self)
{
	self->state = gowl_floatdesk_state_new();
	self->width_pct = FLOATDESK_DEFAULT_WIDTH_PCT;
	self->height_pct = FLOATDESK_DEFAULT_HEIGHT_PCT;
	self->cascade_step = FLOATDESK_DEFAULT_CASCADE;
	self->focus_follows_click = TRUE;
	self->titlebars = TRUE;
	self->titlebar_height = FLOATDESK_DEFAULT_TITLEBAR;
	self->snap_threshold = FLOATDESK_DEFAULT_SNAP;
	self->dock_enabled = TRUE;
	self->dock_icon_size = FLOATDESK_DEFAULT_ICON_SIZE;
	self->terminal = g_strdup(FLOATDESK_DEFAULT_TERMINAL);
	self->monitors = g_hash_table_new_full(g_direct_hash, g_direct_equal,
	                                       NULL, mon_track_free);
}

/* --- Shared-object entry point ----------------------------------- */

G_MODULE_EXPORT GType
gowl_module_register(void)
{
	return GOWL_TYPE_MODULE_FLOATDESK;
}
