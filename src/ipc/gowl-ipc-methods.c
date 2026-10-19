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
 * The gowl wire protocol: the compositor's JSON-RPC methods and the
 * structured form of its events.
 *
 * The text commands in gowl-ipc-core.c are for a person at a shell; this
 * is for a PROGRAM.  The differences are the point of it:
 *
 *   - Every request names its target explicitly (a window id, an output
 *     name) and gets a typed answer or a typed error, instead of an
 *     `OK'/`ERROR' line to pattern-match.
 *   - Parameters are named and checked, so `{"widht": 800}' is an error
 *     rather than a silently ignored key.
 *   - A batch runs inside one event-loop callback, so a program that
 *     lays out six windows gets one frame with all six moved.
 *   - The caller is known (SO_PEERCRED), so "my windows" -- the windows
 *     of the calling process and everything it started -- is a filter,
 *     which is what a program arranging its own child windows wants.
 *   - Events are structured and filterable, and include the ones a
 *     layout program needs that the text dialect never had: a window's
 *     geometry or state changing (`window.changed') and an output's tags
 *     or layout changing from ANY source (`tags.changed',
 *     `layout.changed') -- the text `tags' event is only sent from a few
 *     keybind paths.
 *
 * Every method runs on the compositor's thread -- under `cmacs --gowl'
 * the dispatch thread, holding the gowl lock -- because the socket is
 * dispatched from the compositor's own event loop.  Nothing here may
 * block, and nothing here runs Lisp.
 *
 * docs/ipc.org is the reference; `rpc.discover' asks a running
 * compositor for the same list.
 */

#include "../core/gowl-core-private.h"
#include "../core/gowl-layout-registry.h"
#include "../config/gowl-keybind.h"
#include "gowl-ipc.h"
#include <json-glib/json-glib.h>
#include <glib/gstdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <sys/types.h>

/* How far up the process tree "mine" looks for the caller. */
#define GOWL_IPC_PPID_DEPTH (64)

/* Object data key on the GowlIpc for this file's state. */
#define GOWL_IPC_METHODS_KEY "gowl-ipc-methods-state"

/**
 * GowlIpcWinSnap:
 *
 * What `window.changed' compares: the state of one window as it was
 * last announced.
 */
typedef struct {
	gint     x, y, width, height;
	guint32  tags;
	gchar   *monitor;
	gboolean floating, fullscreen, minimized, sticky, below, urgent;
	gboolean visible;
} GowlIpcWinSnap;

/**
 * GowlIpcMonSnap:
 *
 * What `tags.changed', `layout.changed' and `monitor.changed' compare.
 */
typedef struct {
	guint32  tags;
	gchar   *layout;
	gdouble  mfact;
	gint     nmaster;
	gint     x, y, width, height;
	gint     wx, wy, wwidth, wheight;
	gdouble  scale;
	gint     transform;
	gboolean enabled;
} GowlIpcMonSnap;

/**
 * GowlIpcMethodsState:
 *
 * Per-server state, kept as object data on the #GowlIpc so it lives and
 * dies with the server rather than with the compositor.
 */
typedef struct {
	GWeakRef    compositor;
	GHashTable *windows;    /* guint id -> GowlIpcWinSnap* */
	GHashTable *monitors;   /* gchar* name -> GowlIpcMonSnap* */
} GowlIpcMethodsState;

static void
win_snap_free(gpointer data)
{
	GowlIpcWinSnap *s = (GowlIpcWinSnap *)data;

	g_free(s->monitor);
	g_free(s);
}

static void
mon_snap_free(gpointer data)
{
	GowlIpcMonSnap *s = (GowlIpcMonSnap *)data;

	g_free(s->layout);
	g_free(s);
}

static void
methods_state_free(gpointer data)
{
	GowlIpcMethodsState *st = (GowlIpcMethodsState *)data;

	g_weak_ref_clear(&st->compositor);
	g_hash_table_unref(st->windows);
	g_hash_table_unref(st->monitors);
	g_free(st);
}

/* ── Resolving what a request names ──────────────────────────────── */

/* The compositor a method acts on, or an error once it is gone. */
static GowlCompositor *
comp_get(gpointer data, GError **error)
{
	GowlIpcMethodsState *st = (GowlIpcMethodsState *)data;
	GowlCompositor *c = (GowlCompositor *)g_weak_ref_get(&st->compositor);

	if (c == NULL)
		g_set_error_literal(error, GOWL_IPC_ERROR, GOWL_IPC_ERROR_INTERNAL,
		                    "the compositor is gone");
	return c;
}

static const gchar *
mon_name(GowlMonitor *m)
{
	return m != NULL && m->wlr_output != NULL && m->wlr_output->name != NULL
		? m->wlr_output->name : "";
}

/* Same test as VISIBLEON() in gowl-compositor.c, which is private. */
static gboolean
client_visible(GowlClient *c)
{
	GowlMonitor *m = c->mon;

	if (m == NULL)
		return FALSE;
	if (c->isoverlay)
		return c->overlay_visible;
	return GOWL_CLIENT_PINNED(c)
	       || (c->tags & m->tagset[m->seltags]) != 0;
}

static GowlClient *
client_find(GowlCompositor *self, guint id)
{
	GList *l;

	for (l = self->clients; l != NULL; l = l->next)
		if (((GowlClient *)l->data)->id == id)
			return (GowlClient *)l->data;
	return NULL;
}

/* A dialog's owner: the same rule gowl-compositor.c's client_get_parent()
 * uses, which is private there. */
static GowlClient *
client_parent(GowlClient *c)
{
	GowlClient *p = NULL;

#ifdef GOWL_HAVE_XWAYLAND
	if (c->xwayland_surface != NULL) {
		if (c->xwayland_surface->parent != NULL)
			p = (GowlClient *)c->xwayland_surface->parent->data;
	} else
#endif
	if (c->xdg_toplevel != NULL && c->xdg_toplevel->parent != NULL
	    && c->xdg_toplevel->parent->base != NULL)
		p = (GowlClient *)c->xdg_toplevel->parent->base->data;

	if (p == NULL || p->mon == NULL || p->isunmanaged)
		return NULL;
	return p;
}

/*
 * The window a request names in @key: an id, or "focused" (also what an
 * absent key means when @default_focused).
 */
static GowlClient *
param_window(GowlCompositor *self, JsonObject *params, const gchar *key,
             gboolean default_focused, GError **error)
{
	JsonNode *n = json_object_has_member(params, key)
		? json_object_get_member(params, key) : NULL;
	GowlClient *c;

	if (n == NULL || (JSON_NODE_HOLDS_VALUE(n)
	                  && json_node_get_value_type(n) == G_TYPE_STRING)) {
		if (n == NULL && !default_focused) {
			g_set_error(error, GOWL_IPC_ERROR,
			            GOWL_IPC_ERROR_INVALID_PARAMS, "needs '%s'", key);
			return NULL;
		}
		c = gowl_compositor_get_focused_client(self);
		if (c == NULL)
			g_set_error_literal(error, GOWL_IPC_ERROR,
			                    GOWL_IPC_ERROR_NOT_FOUND,
			                    "no window is focused");
		return c;
	}

	c = client_find(self, (guint)json_node_get_int(n));
	if (c == NULL)
		g_set_error(error, GOWL_IPC_ERROR, GOWL_IPC_ERROR_NOT_FOUND,
		            "no window %" G_GINT64_FORMAT, json_node_get_int(n));
	return c;
}

/* The output a request names in @key: a name, or "focused"/absent. */
static GowlMonitor *
param_monitor(GowlCompositor *self, JsonObject *params, const gchar *key,
              GError **error)
{
	const gchar *name = json_object_get_string_member_with_default(
		params, key, NULL);
	GList *l;

	if (name == NULL || g_strcmp0(name, "focused") == 0) {
		if (self->selmon == NULL)
			g_set_error_literal(error, GOWL_IPC_ERROR,
			                    GOWL_IPC_ERROR_NOT_FOUND,
			                    "there is no output");
		return self->selmon;
	}
	for (l = self->monitors; l != NULL; l = l->next) {
		GowlMonitor *m = (GowlMonitor *)l->data;

		if (g_strcmp0(mon_name(m), name) == 0)
			return m;
	}
	g_set_error(error, GOWL_IPC_ERROR, GOWL_IPC_ERROR_NOT_FOUND,
	            "no output '%s'", name);
	return NULL;
}

/* A TOGGLE parameter: true, false, "toggle", or absent (also toggle). */
static gboolean
param_toggle(JsonObject *params, const gchar *key, gboolean current)
{
	JsonNode *n = json_object_has_member(params, key)
		? json_object_get_member(params, key) : NULL;

	if (n == NULL || json_node_get_value_type(n) == G_TYPE_STRING)
		return !current;
	return json_node_get_boolean(n);
}

static gint64
param_int(JsonObject *params, const gchar *key, gint64 def)
{
	JsonNode *n = json_object_has_member(params, key)
		? json_object_get_member(params, key) : NULL;

	if (n == NULL)
		return def;
	if (json_node_get_value_type(n) == G_TYPE_DOUBLE)
		return (gint64)json_node_get_double(n);
	return json_node_get_int(n);
}

static gdouble
param_double(JsonObject *params, const gchar *key, gdouble def)
{
	JsonNode *n = json_object_has_member(params, key)
		? json_object_get_member(params, key) : NULL;

	if (n == NULL)
		return def;
	if (json_node_get_value_type(n) == G_TYPE_INT64)
		return (gdouble)json_node_get_int(n);
	return json_node_get_double(n);
}

/*
 * A tag set, from either `tags' (a bitmask, as everywhere in gowl) or
 * `tag' (one tag, counted from 1) -- a program that thinks in
 * workspaces should not have to know what a bitmask is.  Returns 0 and
 * sets @error when neither or a bad value is given and @required.
 */
static guint32
param_tags(JsonObject *params, gboolean required, GError **error)
{
	if (json_object_has_member(params, "tags")) {
		gint64 v = param_int(params, "tags", 0);

		if (v <= 0 || v >= ((gint64)1 << GOWL_MAX_TAGS)) {
			g_set_error(error, GOWL_IPC_ERROR,
			            GOWL_IPC_ERROR_INVALID_PARAMS,
			            "'tags' is a non-zero bitmask below 2^%d",
			            GOWL_MAX_TAGS);
			return 0;
		}
		return (guint32)v;
	}
	if (json_object_has_member(params, "tag")) {
		gint64 v = param_int(params, "tag", 0);

		if (v < 1 || v > GOWL_MAX_TAGS) {
			g_set_error(error, GOWL_IPC_ERROR,
			            GOWL_IPC_ERROR_INVALID_PARAMS,
			            "'tag' counts from 1 to %d", GOWL_MAX_TAGS);
			return 0;
		}
		return 1u << (v - 1);
	}
	if (required)
		g_set_error_literal(error, GOWL_IPC_ERROR,
		                    GOWL_IPC_ERROR_INVALID_PARAMS,
		                    "needs 'tags' (a bitmask) or 'tag' (from 1)");
	return 0;
}

/*
 * cmacs's `--gowl' embeds windows in Emacs buffers; Emacs owns their
 * geometry, tags and stacking, and reconciles them on every window
 * change.  Moving one from here would only be undone on the next redisplay,
 * so say why instead of pretending it worked.
 */
static gboolean
refuse_embedded(GowlClient *c, GError **error)
{
	if (!c->isembedded)
		return FALSE;
	g_set_error(error, GOWL_IPC_ERROR, GOWL_IPC_ERROR_REFUSED,
	            "window %u is embedded in an Emacs buffer; cmacs owns its "
	            "place on screen", c->id);
	return TRUE;
}

static gboolean
refuse_locked(GowlCompositor *self, GError **error)
{
	if (!gowl_compositor_is_locked(self))
		return FALSE;
	g_set_error_literal(error, GOWL_IPC_ERROR, GOWL_IPC_ERROR_REFUSED,
	                    "the session is locked");
	return TRUE;
}

/*
 * Reads /proc/PID/stat's ppid.  The comm field may contain spaces and
 * parentheses, so the fields after it are found from the LAST ')'.
 */
static gint
proc_ppid(gint pid)
{
	g_autofree gchar *path = g_strdup_printf("/proc/%d/stat", pid);
	g_autofree gchar *text = NULL;
	const gchar *p;
	gint ppid = -1;

	if (!g_file_get_contents(path, &text, NULL, NULL))
		return -1;
	p = strrchr(text, ')');
	if (p == NULL)
		return -1;
	/* ") S PPID ..." */
	if (sscanf(p + 1, " %*c %d", &ppid) != 1)
		return -1;
	return ppid;
}

/* Whether @pid is @ancestor or descends from it. */
static gboolean
pid_descends(gint pid, gint ancestor)
{
	gint depth;

	if (pid <= 0 || ancestor <= 0)
		return FALSE;
	for (depth = 0; depth < GOWL_IPC_PPID_DEPTH && pid > 1; depth++) {
		if (pid == ancestor)
			return TRUE;
		pid = proc_ppid(pid);
	}
	return pid == ancestor;
}

/* ── JSON forms ──────────────────────────────────────────────────── */

static void
add_window(GowlCompositor *self, JsonBuilder *b, GowlClient *c,
           const GowlIpcCaller *caller)
{
	GowlClient *parent = client_parent(c);
	GowlClient *focused = gowl_compositor_get_focused_client(self);
	pid_t pid = gowl_client_get_pid(c);

	json_builder_begin_object(b);
	json_builder_set_member_name(b, "id");
	json_builder_add_int_value(b, c->id);
	json_builder_set_member_name(b, "app_id");
	json_builder_add_string_value(b, c->app_id != NULL ? c->app_id : "");
	json_builder_set_member_name(b, "title");
	json_builder_add_string_value(b, c->title != NULL ? c->title : "");
	json_builder_set_member_name(b, "pid");
	json_builder_add_int_value(b, pid);
	json_builder_set_member_name(b, "monitor");
	json_builder_add_string_value(b, mon_name(c->mon));
	json_builder_set_member_name(b, "tags");
	json_builder_add_int_value(b, c->isminimized ? c->minimized_tags
	                                              : c->tags);
	json_builder_set_member_name(b, "x");
	json_builder_add_int_value(b, c->geom.x);
	json_builder_set_member_name(b, "y");
	json_builder_add_int_value(b, c->geom.y);
	json_builder_set_member_name(b, "width");
	json_builder_add_int_value(b, c->geom.width);
	json_builder_set_member_name(b, "height");
	json_builder_add_int_value(b, c->geom.height);
	json_builder_set_member_name(b, "border");
	json_builder_add_int_value(b, c->bw);
	json_builder_set_member_name(b, "focused");
	json_builder_add_boolean_value(b, c == focused);
	json_builder_set_member_name(b, "visible");
	json_builder_add_boolean_value(b, client_visible(c));
	json_builder_set_member_name(b, "floating");
	json_builder_add_boolean_value(b, c->isfloating);
	json_builder_set_member_name(b, "fullscreen");
	json_builder_add_boolean_value(b, c->isfullscreen);
	json_builder_set_member_name(b, "urgent");
	json_builder_add_boolean_value(b, c->isurgent);
	json_builder_set_member_name(b, "sticky");
	json_builder_add_boolean_value(b, c->issticky);
	json_builder_set_member_name(b, "pinned");
	json_builder_add_boolean_value(b, GOWL_CLIENT_PINNED(c));
	json_builder_set_member_name(b, "below");
	json_builder_add_boolean_value(b, c->isbelow);
	json_builder_set_member_name(b, "minimized");
	json_builder_add_boolean_value(b, c->isminimized);
	json_builder_set_member_name(b, "embedded");
	json_builder_add_boolean_value(b, c->isembedded);
	json_builder_set_member_name(b, "overlay");
	json_builder_add_boolean_value(b, c->isoverlay);
	json_builder_set_member_name(b, "xwayland");
	json_builder_add_boolean_value(b, gowl_client_get_xwayland(c));
	json_builder_set_member_name(b, "opacity");
	json_builder_add_double_value(b, c->alpha);
	json_builder_set_member_name(b, "parent");
	if (parent != NULL)
		json_builder_add_int_value(b, parent->id);
	else
		json_builder_add_null_value(b);
	json_builder_set_member_name(b, "mine");
	json_builder_add_boolean_value(b,
		caller != NULL && pid_descends((gint)pid, caller->pid));
	json_builder_end_object(b);
}

static JsonNode *
window_node(GowlCompositor *self, GowlClient *c, const GowlIpcCaller *caller)
{
	JsonBuilder *b = json_builder_new();
	JsonNode *n;

	add_window(self, b, c, caller);
	n = json_builder_get_root(b);
	g_object_unref(b);
	return n;
}

static guint32
mon_occupied(GowlCompositor *self, GowlMonitor *m, guint32 *urgent)
{
	guint32 occ = 0;
	GList *l;

	*urgent = 0;
	for (l = self->clients; l != NULL; l = l->next) {
		GowlClient *c = (GowlClient *)l->data;

		if (c->mon != m || c->isembedded || c->isoverlay)
			continue;
		occ |= c->tags;
		if (c->isurgent)
			*urgent |= c->tags;
	}
	return occ;
}

static void
add_box(JsonBuilder *b, const gchar *name, const struct wlr_box *box)
{
	json_builder_set_member_name(b, name);
	json_builder_begin_object(b);
	json_builder_set_member_name(b, "x");
	json_builder_add_int_value(b, box->x);
	json_builder_set_member_name(b, "y");
	json_builder_add_int_value(b, box->y);
	json_builder_set_member_name(b, "width");
	json_builder_add_int_value(b, box->width);
	json_builder_set_member_name(b, "height");
	json_builder_add_int_value(b, box->height);
	json_builder_end_object(b);
}

static void
add_monitor(GowlCompositor *self, JsonBuilder *b, GowlMonitor *m)
{
	GowlLayoutEntry *layout = gowl_layout_get(self, m);
	guint32 urgent;
	guint32 occ = mon_occupied(self, m, &urgent);
	struct wlr_output *o = m->wlr_output;

	json_builder_begin_object(b);
	json_builder_set_member_name(b, "name");
	json_builder_add_string_value(b, mon_name(m));
	json_builder_set_member_name(b, "make");
	json_builder_add_string_value(b, o != NULL && o->make != NULL ? o->make : "");
	json_builder_set_member_name(b, "model");
	json_builder_add_string_value(b, o != NULL && o->model != NULL ? o->model : "");
	json_builder_set_member_name(b, "serial");
	json_builder_add_string_value(b, o != NULL && o->serial != NULL ? o->serial : "");
	json_builder_set_member_name(b, "focused");
	json_builder_add_boolean_value(b, m == self->selmon);
	json_builder_set_member_name(b, "enabled");
	json_builder_add_boolean_value(b, o != NULL && o->enabled);
	json_builder_set_member_name(b, "powered_off");
	json_builder_add_boolean_value(b, m->powered_off);
	json_builder_set_member_name(b, "x");
	json_builder_add_int_value(b, m->m.x);
	json_builder_set_member_name(b, "y");
	json_builder_add_int_value(b, m->m.y);
	json_builder_set_member_name(b, "width");
	json_builder_add_int_value(b, m->m.width);
	json_builder_set_member_name(b, "height");
	json_builder_add_int_value(b, m->m.height);
	/* The window area: the output less bars and other exclusive
	 * layer surfaces.  What a program laying windows out wants. */
	add_box(b, "area", &m->w);
	json_builder_set_member_name(b, "scale");
	json_builder_add_double_value(b, o != NULL ? o->scale : 1.0);
	json_builder_set_member_name(b, "transform");
	json_builder_add_int_value(b, o != NULL ? (gint64)o->transform : 0);
	json_builder_set_member_name(b, "refresh");
	json_builder_add_double_value(b,
		o != NULL ? (gdouble)o->refresh / 1000.0 : 0.0);
	json_builder_set_member_name(b, "hdr");
	json_builder_add_boolean_value(b, gowl_monitor_get_hdr(m));
	json_builder_set_member_name(b, "tags");
	json_builder_add_int_value(b, m->tagset[m->seltags]);
	json_builder_set_member_name(b, "occupied");
	json_builder_add_int_value(b, occ);
	json_builder_set_member_name(b, "urgent");
	json_builder_add_int_value(b, urgent);
	json_builder_set_member_name(b, "layout");
	json_builder_add_string_value(b,
		layout != NULL && layout->name != NULL ? layout->name : "");
	json_builder_set_member_name(b, "layout_symbol");
	json_builder_add_string_value(b,
		layout != NULL && layout->symbol != NULL ? layout->symbol : "");
	json_builder_set_member_name(b, "mfact");
	json_builder_add_double_value(b, m->mfact);
	json_builder_set_member_name(b, "nmaster");
	json_builder_add_int_value(b, m->nmaster);
	json_builder_end_object(b);
}

static JsonNode *
monitor_node(GowlCompositor *self, GowlMonitor *m)
{
	JsonBuilder *b = json_builder_new();
	JsonNode *n;

	add_monitor(self, b, m);
	n = json_builder_get_root(b);
	g_object_unref(b);
	return n;
}

static JsonNode *
builder_finish(JsonBuilder *b)
{
	JsonNode *n = json_builder_get_root(b);

	g_object_unref(b);
	return n;
}

static JsonNode *
ok_node(void)
{
	JsonObject *o = json_object_new();
	JsonNode *n = json_node_new(JSON_NODE_OBJECT);

	json_object_set_boolean_member(o, "ok", TRUE);
	json_node_take_object(n, o);
	return n;
}

static JsonNode *
bool_node(gboolean v)
{
	JsonNode *n = json_node_new(JSON_NODE_VALUE);

	json_node_set_boolean(n, v);
	return n;
}

static JsonNode *
string_node(const gchar *s)
{
	JsonNode *n = json_node_new(JSON_NODE_VALUE);

	json_node_set_string(n, s != NULL ? s : "");
	return n;
}

/* ── Geometry arithmetic ─────────────────────────────────────────── */

/*
 * Turns a requested rectangle into layout coordinates.
 *
 * @relative_to:  "layout"  coordinates are already layout coordinates
 *                "monitor" relative to the output's top-left
 *                "area"    relative to the window area (the output less
 *                          its bars) -- the default for a program that
 *                          lays windows out, since 0,0 then never sits
 *                          under a bar
 * @units:        "px" or "fraction" (of the monitor or area box, so
 *                {x:0.5, width:0.5} is the right half)
 *
 * Members not given keep @cur's value.
 */
static gboolean
resolve_box(JsonObject *spec, GowlMonitor *m, const struct wlr_box *cur,
            const gchar *relative_to, const gchar *units,
            struct wlr_box *out, GError **error)
{
	const struct wlr_box *ref;
	struct wlr_box zero = { 0, 0, 0, 0 };
	gboolean fraction;
	gdouble x, y, w, h;

	if (relative_to == NULL)
		relative_to = "layout";
	if (units == NULL)
		units = "px";

	if (g_strcmp0(relative_to, "layout") == 0)
		ref = &zero;
	else if (g_strcmp0(relative_to, "monitor") == 0 && m != NULL)
		ref = &m->m;
	else if (g_strcmp0(relative_to, "area") == 0 && m != NULL)
		ref = &m->w;
	else {
		g_set_error(error, GOWL_IPC_ERROR, GOWL_IPC_ERROR_INVALID_PARAMS,
		            "relative_to is layout, monitor or area, not '%s'",
		            relative_to);
		return FALSE;
	}

	if (g_strcmp0(units, "px") == 0)
		fraction = FALSE;
	else if (g_strcmp0(units, "fraction") == 0)
		fraction = TRUE;
	else {
		g_set_error(error, GOWL_IPC_ERROR, GOWL_IPC_ERROR_INVALID_PARAMS,
		            "units is px or fraction, not '%s'", units);
		return FALSE;
	}
	if (fraction && ref == &zero) {
		/* A fraction of the whole layout is not a thing anybody
		 * means; take it as a fraction of the window's output. */
		if (m == NULL) {
			g_set_error_literal(error, GOWL_IPC_ERROR,
			                    GOWL_IPC_ERROR_INVALID_PARAMS,
			                    "fractions need an output to be a "
			                    "fraction of");
			return FALSE;
		}
		ref = &m->m;
	}

	/* Current values, expressed in the request's own terms, so an
	 * absent member means "unchanged". */
	x = (gdouble)(cur->x - ref->x);
	y = (gdouble)(cur->y - ref->y);
	w = (gdouble)cur->width;
	h = (gdouble)cur->height;
	if (fraction) {
		x = ref->width > 0 ? x / ref->width : 0.0;
		y = ref->height > 0 ? y / ref->height : 0.0;
		w = ref->width > 0 ? w / ref->width : 0.0;
		h = ref->height > 0 ? h / ref->height : 0.0;
	}
	x = param_double(spec, "x", x);
	y = param_double(spec, "y", y);
	w = param_double(spec, "width", w);
	h = param_double(spec, "height", h);
	if (fraction) {
		x *= ref->width;
		y *= ref->height;
		w *= ref->width;
		h *= ref->height;
	}

	out->x = ref->x + (gint)lround(x);
	out->y = ref->y + (gint)lround(y);
	out->width = (gint)lround(w);
	out->height = (gint)lround(h);
	if (out->width < 1 || out->height < 1) {
		g_set_error(error, GOWL_IPC_ERROR, GOWL_IPC_ERROR_INVALID_PARAMS,
		            "a window needs a size of at least 1x1, not %dx%d",
		            out->width, out->height);
		return FALSE;
	}
	return TRUE;
}

/*
 * Puts a window at @box, floating it first: a tiled window's place is
 * the layout's to decide, and the next arrange would put it back.
 */
static void
place_window(GowlCompositor *self, GowlClient *c, const struct wlr_box *box)
{
	if (c->isfullscreen)
		gowl_compositor_set_client_fullscreen(self, c, FALSE);
	if (!c->isfloating)
		gowl_compositor_set_floating(self, c, TRUE);
	gowl_compositor_resize_client(self, c, box->x, box->y,
	                              box->width, box->height);
}

/* ── gowl.* ──────────────────────────────────────────────────────── */

static JsonNode *
m_gowl_version(GowlIpc *ipc, const GowlIpcCaller *caller, JsonObject *p,
               gpointer data, GError **error)
{
	JsonBuilder *b = json_builder_new();

	(void)ipc; (void)caller; (void)p; (void)data; (void)error;
	json_builder_begin_object(b);
	json_builder_set_member_name(b, "version");
	json_builder_add_string_value(b, GOWL_VERSION);
	json_builder_set_member_name(b, "protocol");
	json_builder_add_string_value(b, GOWL_IPC_PROTOCOL_NAME);
	json_builder_set_member_name(b, "protocol_version");
	json_builder_add_int_value(b, GOWL_IPC_PROTOCOL_VERSION);
	json_builder_set_member_name(b, "wlroots");
	json_builder_add_string_value(b,
		G_STRINGIFY(GOWL_WLROOTS_VERSION_MAJOR) "."
		G_STRINGIFY(GOWL_WLROOTS_VERSION_MINOR));
	json_builder_set_member_name(b, "xwayland");
#ifdef GOWL_HAVE_XWAYLAND
	json_builder_add_boolean_value(b, TRUE);
#else
	json_builder_add_boolean_value(b, FALSE);
#endif
	json_builder_end_object(b);
	return builder_finish(b);
}

/*
 * gowl.command: any text command, from JSON.  The escape hatch to
 * everything that has a text command and no method -- every module
 * command (`expo', `scratchpad-toggle', `macro-run ...') included.
 */
static JsonNode *
m_gowl_command(GowlIpc *ipc, const GowlIpcCaller *caller, JsonObject *p,
               gpointer data, GError **error)
{
	g_autoptr(GowlCompositor) self = comp_get(data, error);
	const gchar *line = json_object_get_string_member(p, "line");
	g_autofree gchar *reply = NULL;
	JsonBuilder *b;
	gboolean ok;

	(void)ipc; (void)caller;
	if (self == NULL)
		return NULL;
	if (*line == '{' || *line == '[') {
		g_set_error_literal(error, GOWL_IPC_ERROR,
		                    GOWL_IPC_ERROR_INVALID_PARAMS,
		                    "gowl.command takes a text command, not JSON");
		return NULL;
	}
	reply = gowl_compositor_ipc_command(self, line);
	if (reply == NULL) {
		g_set_error(error, GOWL_IPC_ERROR, GOWL_IPC_ERROR_METHOD_NOT_FOUND,
		            "unknown command '%s'", line);
		return NULL;
	}
	ok = !g_str_has_prefix(reply, "ERROR");

	b = json_builder_new();
	json_builder_begin_object(b);
	json_builder_set_member_name(b, "ok");
	json_builder_add_boolean_value(b, ok);
	json_builder_set_member_name(b, "reply");
	json_builder_add_string_value(b, reply);
	/* A query's reply is JSON already; hand it over parsed, so the
	 * caller does not decode a string inside a string. */
	if (reply[0] == '{' || reply[0] == '[' || g_strcmp0(reply, "null") == 0) {
		g_autoptr(JsonParser) parser = json_parser_new_immutable();

		if (json_parser_load_from_data(parser, reply, -1, NULL)) {
			json_builder_set_member_name(b, "json");
			json_builder_add_value(b,
				json_node_copy(json_parser_get_root(parser)));
		}
	}
	json_builder_end_object(b);
	return builder_finish(b);
}

static JsonNode *
m_gowl_reload(GowlIpc *ipc, const GowlIpcCaller *caller, JsonObject *p,
              gpointer data, GError **error)
{
	g_autoptr(GowlCompositor) self = comp_get(data, error);
	GowlKeybindEntry kb;
	JsonBuilder *b;

	(void)ipc; (void)caller; (void)p;
	if (self == NULL)
		return NULL;
	memset(&kb, 0, sizeof kb);
	kb.action = GOWL_ACTION_RELOAD_CONFIG;
	gowl_compositor_run_keybind_entry(self, &kb);

	b = json_builder_new();
	json_builder_begin_object(b);
	json_builder_set_member_name(b, "problems");
	json_builder_add_int_value(b, self->config != NULL
		? gowl_config_get_problem_count(self->config) : 0);
	json_builder_end_object(b);
	return builder_finish(b);
}

/* gowl.state: everything a program needs to start, in one round trip. */
static JsonNode *
m_gowl_state(GowlIpc *ipc, const GowlIpcCaller *caller, JsonObject *p,
             gpointer data, GError **error)
{
	g_autoptr(GowlCompositor) self = comp_get(data, error);
	GowlClient *focused;
	JsonBuilder *b;
	GList *l;
	guint idx = 0;
	const gchar *kname;

	(void)ipc; (void)p;
	if (self == NULL)
		return NULL;

	focused = gowl_compositor_get_focused_client(self);
	kname = gowl_compositor_get_keyboard_layout(self, &idx);

	b = json_builder_new();
	json_builder_begin_object(b);
	json_builder_set_member_name(b, "version");
	json_builder_add_string_value(b, GOWL_VERSION);
	json_builder_set_member_name(b, "focused_monitor");
	json_builder_add_string_value(b, mon_name(self->selmon));
	json_builder_set_member_name(b, "focused_window");
	if (focused != NULL)
		json_builder_add_int_value(b, focused->id);
	else
		json_builder_add_null_value(b);
	json_builder_set_member_name(b, "locked");
	json_builder_add_boolean_value(b, gowl_compositor_is_locked(self));
	json_builder_set_member_name(b, "keymode");
	json_builder_add_string_value(b, gowl_compositor_get_key_mode(self));
	json_builder_set_member_name(b, "keyboard_layout");
	json_builder_begin_object(b);
	json_builder_set_member_name(b, "name");
	json_builder_add_string_value(b, kname != NULL ? kname : "");
	json_builder_set_member_name(b, "index");
	json_builder_add_int_value(b, idx);
	json_builder_end_object(b);
	json_builder_set_member_name(b, "monitors");
	json_builder_begin_array(b);
	for (l = self->monitors; l != NULL; l = l->next)
		add_monitor(self, b, (GowlMonitor *)l->data);
	json_builder_end_array(b);
	json_builder_set_member_name(b, "windows");
	json_builder_begin_array(b);
	for (l = self->clients; l != NULL; l = l->next)
		add_window(self, b, (GowlClient *)l->data, caller);
	json_builder_end_array(b);
	json_builder_end_object(b);
	return builder_finish(b);
}

/* ── windows.* / window.* ────────────────────────────────────────── */

/* windows.list: every window, optionally filtered. */
static JsonNode *
m_windows_list(GowlIpc *ipc, const GowlIpcCaller *caller, JsonObject *p,
               gpointer data, GError **error)
{
	g_autoptr(GowlCompositor) self = comp_get(data, error);
	const gchar *app_id = json_object_get_string_member_with_default(
		p, "app_id", NULL);
	const gchar *title = json_object_get_string_member_with_default(
		p, "title", NULL);
	const gchar *monitor = json_object_get_string_member_with_default(
		p, "monitor", NULL);
	gint64 pid = param_int(p, "pid", 0);
	gint64 tree = param_int(p, "descendants_of", 0);
	gboolean mine = json_object_get_boolean_member_with_default(
		p, "mine", FALSE);
	gboolean visible_only = json_object_get_boolean_member_with_default(
		p, "visible", FALSE);
	gboolean has_floating = json_object_has_member(p, "floating");
	gboolean floating = json_object_get_boolean_member_with_default(
		p, "floating", FALSE);
	gboolean with_embedded = json_object_get_boolean_member_with_default(
		p, "embedded", TRUE);
	guint32 tags = 0;
	JsonBuilder *b;
	GList *l;

	(void)ipc;
	if (self == NULL)
		return NULL;
	if (json_object_has_member(p, "tags") || json_object_has_member(p, "tag")) {
		tags = param_tags(p, TRUE, error);
		if (tags == 0)
			return NULL;
	}
	if (mine && caller->pid <= 0) {
		g_set_error_literal(error, GOWL_IPC_ERROR, GOWL_IPC_ERROR_REFUSED,
		                    "the caller's pid is unknown, so 'mine' "
		                    "cannot be answered");
		return NULL;
	}

	b = json_builder_new();
	json_builder_begin_array(b);
	for (l = self->clients; l != NULL; l = l->next) {
		GowlClient *c = (GowlClient *)l->data;
		gint cpid = (gint)gowl_client_get_pid(c);
		guint32 ctags = c->isminimized ? c->minimized_tags : c->tags;

		if (!with_embedded && c->isembedded)
			continue;
		if (app_id != NULL && g_strcmp0(c->app_id, app_id) != 0)
			continue;
		if (title != NULL
		    && (c->title == NULL || strstr(c->title, title) == NULL))
			continue;
		if (monitor != NULL && g_strcmp0(mon_name(c->mon), monitor) != 0)
			continue;
		if (pid > 0 && cpid != (gint)pid)
			continue;
		if (tree > 0 && !pid_descends(cpid, (gint)tree))
			continue;
		if (mine && !pid_descends(cpid, caller->pid))
			continue;
		if (tags != 0 && (ctags & tags) == 0)
			continue;
		if (visible_only && !client_visible(c))
			continue;
		if (has_floating && c->isfloating != floating)
			continue;
		add_window(self, b, c, caller);
	}
	json_builder_end_array(b);
	return builder_finish(b);
}

static JsonNode *
m_window_get(GowlIpc *ipc, const GowlIpcCaller *caller, JsonObject *p,
             gpointer data, GError **error)
{
	g_autoptr(GowlCompositor) self = comp_get(data, error);
	GowlClient *c;

	(void)ipc;
	if (self == NULL)
		return NULL;
	c = param_window(self, p, "id", TRUE, error);
	return c != NULL ? window_node(self, c, caller) : NULL;
}

static JsonNode *
m_window_focused(GowlIpc *ipc, const GowlIpcCaller *caller, JsonObject *p,
                 gpointer data, GError **error)
{
	g_autoptr(GowlCompositor) self = comp_get(data, error);
	GowlClient *c;

	(void)ipc; (void)p;
	if (self == NULL)
		return NULL;
	c = gowl_compositor_get_focused_client(self);
	return c != NULL ? window_node(self, c, caller)
	                 : json_node_new(JSON_NODE_NULL);
}

static JsonNode *
m_window_focus(GowlIpc *ipc, const GowlIpcCaller *caller, JsonObject *p,
               gpointer data, GError **error)
{
	g_autoptr(GowlCompositor) self = comp_get(data, error);
	GowlClient *c;

	(void)ipc;
	if (self == NULL || refuse_locked(self, error))
		return NULL;
	c = param_window(self, p, "id", FALSE, error);
	if (c == NULL)
		return NULL;
	/* Through activation, which switches to the window's tag and
	 * output, un-minimizes it, and honours focus-on-activate -- the
	 * same thing a notification's "show" does. */
	gowl_compositor_activate_client(self, c);
	return window_node(self, c, caller);
}

static JsonNode *
m_window_close(GowlIpc *ipc, const GowlIpcCaller *caller, JsonObject *p,
               gpointer data, GError **error)
{
	g_autoptr(GowlCompositor) self = comp_get(data, error);
	GowlClient *c;

	(void)ipc; (void)caller;
	if (self == NULL)
		return NULL;
	c = param_window(self, p, "id", FALSE, error);
	if (c == NULL)
		return NULL;
	gowl_client_close(c);
	return ok_node();
}

static const gchar *
str_member(JsonObject *p, const gchar *key, const gchar *def)
{
	return json_object_get_string_member_with_default(p, key, def);
}

/* window.set_geometry: float a window and put it exactly here. */
static JsonNode *
m_window_set_geometry(GowlIpc *ipc, const GowlIpcCaller *caller,
                      JsonObject *p, gpointer data, GError **error)
{
	g_autoptr(GowlCompositor) self = comp_get(data, error);
	GowlClient *c;
	struct wlr_box box;

	(void)ipc;
	if (self == NULL)
		return NULL;
	c = param_window(self, p, "id", TRUE, error);
	if (c == NULL || refuse_embedded(c, error))
		return NULL;
	if (!resolve_box(p, c->mon, &c->geom,
	                 str_member(p, "relative_to", "layout"),
	                 str_member(p, "units", "px"), &box, error))
		return NULL;
	place_window(self, c, &box);
	return window_node(self, c, caller);
}

/* window.move_by: nudge and/or grow a window by deltas. */
static JsonNode *
m_window_move_by(GowlIpc *ipc, const GowlIpcCaller *caller, JsonObject *p,
                 gpointer data, GError **error)
{
	g_autoptr(GowlCompositor) self = comp_get(data, error);
	GowlClient *c;
	struct wlr_box box;

	(void)ipc;
	if (self == NULL)
		return NULL;
	c = param_window(self, p, "id", TRUE, error);
	if (c == NULL || refuse_embedded(c, error))
		return NULL;
	box = c->geom;
	box.x += (gint)param_int(p, "dx", 0);
	box.y += (gint)param_int(p, "dy", 0);
	box.width += (gint)param_int(p, "dwidth", 0);
	box.height += (gint)param_int(p, "dheight", 0);
	if (box.width < 1 || box.height < 1) {
		g_set_error_literal(error, GOWL_IPC_ERROR,
		                    GOWL_IPC_ERROR_INVALID_PARAMS,
		                    "that would leave the window smaller than 1x1");
		return NULL;
	}
	place_window(self, c, &box);
	return window_node(self, c, caller);
}

/* window.center: float a window centred on its output's window area. */
static JsonNode *
m_window_center(GowlIpc *ipc, const GowlIpcCaller *caller, JsonObject *p,
                gpointer data, GError **error)
{
	g_autoptr(GowlCompositor) self = comp_get(data, error);
	GowlClient *c;
	struct wlr_box box;

	(void)ipc;
	if (self == NULL)
		return NULL;
	c = param_window(self, p, "id", TRUE, error);
	if (c == NULL || refuse_embedded(c, error))
		return NULL;
	if (c->mon == NULL) {
		g_set_error_literal(error, GOWL_IPC_ERROR, GOWL_IPC_ERROR_REFUSED,
		                    "the window is on no output");
		return NULL;
	}
	box.width = (gint)param_int(p, "width", c->geom.width);
	box.height = (gint)param_int(p, "height", c->geom.height);
	if (box.width < 1 || box.height < 1) {
		g_set_error_literal(error, GOWL_IPC_ERROR,
		                    GOWL_IPC_ERROR_INVALID_PARAMS,
		                    "a window needs a size of at least 1x1");
		return NULL;
	}
	box.x = c->mon->w.x + (c->mon->w.width - box.width) / 2;
	box.y = c->mon->w.y + (c->mon->w.height - box.height) / 2;
	place_window(self, c, &box);
	return window_node(self, c, caller);
}

/*
 * The boolean setters share a shape: resolve the window, refuse the
 * embedded ones where the state is Emacs's, work out the new value from
 * the toggle, apply it.
 */
typedef enum {
	WIN_FLAG_FLOATING,
	WIN_FLAG_FULLSCREEN,
	WIN_FLAG_STICKY,
	WIN_FLAG_BELOW,
	WIN_FLAG_MINIMIZED,
	WIN_FLAG_URGENT
} WinFlag;

static JsonNode *
set_flag(gpointer data, const GowlIpcCaller *caller, JsonObject *p,
         WinFlag flag, GError **error)
{
	g_autoptr(GowlCompositor) self = comp_get(data, error);
	GowlClient *c;
	gboolean want;

	if (self == NULL)
		return NULL;
	c = param_window(self, p, "id", TRUE, error);
	if (c == NULL)
		return NULL;
	if (flag != WIN_FLAG_URGENT && refuse_embedded(c, error))
		return NULL;
	if (c->isoverlay && flag != WIN_FLAG_URGENT) {
		g_set_error(error, GOWL_IPC_ERROR, GOWL_IPC_ERROR_REFUSED,
		            "window %u is an overlay (scratchpad, dropdown); its "
		            "module owns its state", c->id);
		return NULL;
	}

	switch (flag) {
	case WIN_FLAG_FLOATING:
		want = param_toggle(p, "value", c->isfloating);
		if (want != c->isfloating)
			gowl_compositor_set_floating(self, c, want);
		break;
	case WIN_FLAG_FULLSCREEN:
		want = param_toggle(p, "value", c->isfullscreen);
		if (want != c->isfullscreen)
			gowl_compositor_set_client_fullscreen(self, c, want);
		break;
	case WIN_FLAG_STICKY:
		want = param_toggle(p, "value", c->issticky);
		/* Pinning only means something while floating; a request to
		 * pin is a request for the window to follow the user, so it
		 * floats too rather than silently doing nothing. */
		if (want && !c->isfloating)
			gowl_compositor_set_floating(self, c, TRUE);
		gowl_client_set_sticky(c, want);
		if (c->mon != NULL)
			gowl_compositor_arrange(self, c->mon);
		break;
	case WIN_FLAG_BELOW:
		want = param_toggle(p, "value", c->isbelow);
		if (want && !c->isfloating)
			gowl_compositor_set_floating(self, c, TRUE);
		gowl_compositor_set_client_below(self, c, want);
		break;
	case WIN_FLAG_MINIMIZED:
		want = param_toggle(p, "value", c->isminimized);
		if (want && !c->isminimized) {
			if (!gowl_compositor_minimize_client(self, c)) {
				g_set_error(error, GOWL_IPC_ERROR,
				            GOWL_IPC_ERROR_REFUSED,
				            "window %u cannot be minimized", c->id);
				return NULL;
			}
		} else if (!want && c->isminimized) {
			GowlMonitor *m = NULL;

			if (json_object_has_member(p, "monitor")) {
				m = param_monitor(self, p, "monitor", error);
				if (m == NULL)
					return NULL;
			}
			if (!gowl_compositor_unminimize_client(self, c, m,
			                                       param_tags(p, FALSE,
			                                                  NULL))) {
				g_set_error(error, GOWL_IPC_ERROR,
				            GOWL_IPC_ERROR_FAILED,
				            "window %u could not be restored", c->id);
				return NULL;
			}
		}
		break;
	case WIN_FLAG_URGENT:
		want = param_toggle(p, "value", c->isurgent);
		gowl_client_set_urgent(c, want);
		break;
	default:
		break;
	}
	return window_node(self, c, caller);
}

static JsonNode *
m_window_set_floating(GowlIpc *ipc, const GowlIpcCaller *caller,
                      JsonObject *p, gpointer data, GError **error)
{
	(void)ipc;
	return set_flag(data, caller, p, WIN_FLAG_FLOATING, error);
}

static JsonNode *
m_window_set_fullscreen(GowlIpc *ipc, const GowlIpcCaller *caller,
                        JsonObject *p, gpointer data, GError **error)
{
	(void)ipc;
	return set_flag(data, caller, p, WIN_FLAG_FULLSCREEN, error);
}

static JsonNode *
m_window_set_sticky(GowlIpc *ipc, const GowlIpcCaller *caller,
                    JsonObject *p, gpointer data, GError **error)
{
	(void)ipc;
	return set_flag(data, caller, p, WIN_FLAG_STICKY, error);
}

static JsonNode *
m_window_set_below(GowlIpc *ipc, const GowlIpcCaller *caller,
                   JsonObject *p, gpointer data, GError **error)
{
	(void)ipc;
	return set_flag(data, caller, p, WIN_FLAG_BELOW, error);
}

static JsonNode *
m_window_set_minimized(GowlIpc *ipc, const GowlIpcCaller *caller,
                       JsonObject *p, gpointer data, GError **error)
{
	(void)ipc;
	return set_flag(data, caller, p, WIN_FLAG_MINIMIZED, error);
}

static JsonNode *
m_window_set_urgent(GowlIpc *ipc, const GowlIpcCaller *caller,
                    JsonObject *p, gpointer data, GError **error)
{
	(void)ipc;
	return set_flag(data, caller, p, WIN_FLAG_URGENT, error);
}

static JsonNode *
m_window_set_opacity(GowlIpc *ipc, const GowlIpcCaller *caller,
                     JsonObject *p, gpointer data, GError **error)
{
	g_autoptr(GowlCompositor) self = comp_get(data, error);
	GowlClient *c;
	gdouble v;

	(void)ipc;
	if (self == NULL)
		return NULL;
	c = param_window(self, p, "id", TRUE, error);
	if (c == NULL)
		return NULL;
	v = param_double(p, "opacity", 1.0);
	if (v < 0.0 || v > 1.0) {
		g_set_error(error, GOWL_IPC_ERROR, GOWL_IPC_ERROR_INVALID_PARAMS,
		            "opacity is 0.0 to 1.0, not %g", v);
		return NULL;
	}
	gowl_client_set_alpha(c, (gfloat)v);
	return window_node(self, c, caller);
}

/* window.set_tags: put a window on tags of its own output. */
static JsonNode *
m_window_set_tags(GowlIpc *ipc, const GowlIpcCaller *caller, JsonObject *p,
                  gpointer data, GError **error)
{
	g_autoptr(GowlCompositor) self = comp_get(data, error);
	GowlClient *c;
	guint32 tags;

	(void)ipc;
	if (self == NULL)
		return NULL;
	c = param_window(self, p, "id", TRUE, error);
	if (c == NULL || refuse_embedded(c, error))
		return NULL;
	tags = param_tags(p, TRUE, error);
	if (tags == 0)
		return NULL;
	if (c->isminimized) {
		/* Restored onto the tags asked for, not left hidden on
		 * "no tag" with a different remembered set. */
		gowl_compositor_unminimize_client(self, c, NULL, tags);
	} else {
		gowl_compositor_move_client(self, c, NULL, tags);
	}
	return window_node(self, c, caller);
}

/* window.send_to_monitor: move a window to another output. */
static JsonNode *
m_window_send_to_monitor(GowlIpc *ipc, const GowlIpcCaller *caller,
                         JsonObject *p, gpointer data, GError **error)
{
	g_autoptr(GowlCompositor) self = comp_get(data, error);
	GowlClient *c;
	GowlMonitor *m;
	guint32 tags;

	(void)ipc;
	if (self == NULL)
		return NULL;
	c = param_window(self, p, "id", TRUE, error);
	if (c == NULL || refuse_embedded(c, error))
		return NULL;
	if (!json_object_has_member(p, "monitor")) {
		g_set_error_literal(error, GOWL_IPC_ERROR,
		                    GOWL_IPC_ERROR_INVALID_PARAMS,
		                    "window.send_to_monitor needs 'monitor'");
		return NULL;
	}
	m = param_monitor(self, p, "monitor", error);
	if (m == NULL)
		return NULL;
	/* Default: whatever that output is showing, so the window
	 * arrives somewhere visible. */
	tags = param_tags(p, FALSE, NULL);
	if (tags == 0)
		tags = m->tagset[m->seltags];
	gowl_compositor_move_client(self, c, m, tags);
	return window_node(self, c, caller);
}

static JsonNode *
m_window_swap(GowlIpc *ipc, const GowlIpcCaller *caller, JsonObject *p,
              gpointer data, GError **error)
{
	g_autoptr(GowlCompositor) self = comp_get(data, error);
	GowlClient *a, *b;

	(void)ipc; (void)caller;
	if (self == NULL)
		return NULL;
	a = param_window(self, p, "id", TRUE, error);
	if (a == NULL)
		return NULL;
	b = param_window(self, p, "with", FALSE, error);
	if (b == NULL || refuse_embedded(a, error) || refuse_embedded(b, error))
		return NULL;
	if (a == b)
		return ok_node();
	gowl_compositor_swap_clients(self, a, b);
	return ok_node();
}

static JsonNode *
m_window_promote(GowlIpc *ipc, const GowlIpcCaller *caller, JsonObject *p,
                 gpointer data, GError **error)
{
	g_autoptr(GowlCompositor) self = comp_get(data, error);
	GowlClient *c;

	(void)ipc;
	if (self == NULL)
		return NULL;
	c = param_window(self, p, "id", TRUE, error);
	if (c == NULL || refuse_embedded(c, error))
		return NULL;
	gowl_compositor_zoom_client(self, c);
	return window_node(self, c, caller);
}

static JsonNode *
m_window_at(GowlIpc *ipc, const GowlIpcCaller *caller, JsonObject *p,
            gpointer data, GError **error)
{
	g_autoptr(GowlCompositor) self = comp_get(data, error);
	GowlClient *c;

	(void)ipc;
	if (self == NULL)
		return NULL;
	c = gowl_compositor_client_at(self, param_double(p, "x", 0.0),
	                              param_double(p, "y", 0.0));
	return c != NULL ? window_node(self, c, caller)
	                 : json_node_new(JSON_NODE_NULL);
}

static gboolean
direction_parse(const gchar *s, GowlDirection *out)
{
	if (g_ascii_strcasecmp(s, "left") == 0)
		*out = GOWL_DIRECTION_LEFT;
	else if (g_ascii_strcasecmp(s, "right") == 0)
		*out = GOWL_DIRECTION_RIGHT;
	else if (g_ascii_strcasecmp(s, "up") == 0)
		*out = GOWL_DIRECTION_UP;
	else if (g_ascii_strcasecmp(s, "down") == 0)
		*out = GOWL_DIRECTION_DOWN;
	else
		return FALSE;
	return TRUE;
}

static JsonNode *
m_window_neighbour(GowlIpc *ipc, const GowlIpcCaller *caller, JsonObject *p,
                   gpointer data, GError **error)
{
	g_autoptr(GowlCompositor) self = comp_get(data, error);
	GowlClient *c, *n;
	GowlDirection dir;

	(void)ipc;
	if (self == NULL)
		return NULL;
	c = param_window(self, p, "id", TRUE, error);
	if (c == NULL)
		return NULL;
	if (!direction_parse(json_object_get_string_member(p, "direction"),
	                     &dir)) {
		g_set_error_literal(error, GOWL_IPC_ERROR,
		                    GOWL_IPC_ERROR_INVALID_PARAMS,
		                    "direction is left, right, up or down");
		return NULL;
	}
	n = gowl_compositor_direction_neighbour(self, c, dir);
	return n != NULL ? window_node(self, n, caller)
	                 : json_node_new(JSON_NODE_NULL);
}

/* window.children: the dialogs a window owns (transient-for). */
static JsonNode *
m_window_children(GowlIpc *ipc, const GowlIpcCaller *caller, JsonObject *p,
                  gpointer data, GError **error)
{
	g_autoptr(GowlCompositor) self = comp_get(data, error);
	GowlClient *parent;
	gboolean deep = json_object_get_boolean_member_with_default(
		p, "recursive", FALSE);
	JsonBuilder *b;
	GList *l;

	(void)ipc;
	if (self == NULL)
		return NULL;
	parent = param_window(self, p, "id", TRUE, error);
	if (parent == NULL)
		return NULL;

	b = json_builder_new();
	json_builder_begin_array(b);
	for (l = self->clients; l != NULL; l = l->next) {
		GowlClient *c = (GowlClient *)l->data;
		GowlClient *up = client_parent(c);
		gint guard = 0;

		/* Recursive: any ancestor is the parent.  The guard is for
		 * a client that names itself, which nothing prevents. */
		while (up != NULL && up != parent && deep && guard++ < 32)
			up = client_parent(up);
		if (up == parent && c != parent)
			add_window(self, b, c, caller);
	}
	json_builder_end_array(b);
	return builder_finish(b);
}

static JsonNode *
m_window_process(GowlIpc *ipc, const GowlIpcCaller *caller, JsonObject *p,
                 gpointer data, GError **error)
{
	g_autoptr(GowlCompositor) self = comp_get(data, error);
	GowlClient *c;
	GowlProcessInfo *info;
	JsonBuilder *b;
	gint ppid;

	(void)ipc; (void)caller;
	if (self == NULL)
		return NULL;
	c = param_window(self, p, "id", TRUE, error);
	if (c == NULL)
		return NULL;
	info = gowl_client_get_process_info(c);
	if (info == NULL) {
		g_set_error(error, GOWL_IPC_ERROR, GOWL_IPC_ERROR_NOT_FOUND,
		            "window %u's process is unknown", c->id);
		return NULL;
	}
	ppid = proc_ppid((gint)info->pid);

	b = json_builder_new();
	json_builder_begin_object(b);
	json_builder_set_member_name(b, "pid");
	json_builder_add_int_value(b, info->pid);
	json_builder_set_member_name(b, "ppid");
	json_builder_add_int_value(b, ppid);
	json_builder_set_member_name(b, "comm");
	json_builder_add_string_value(b, info->comm != NULL ? info->comm : "");
	json_builder_set_member_name(b, "cmdline");
	json_builder_add_string_value(b, info->cmdline != NULL ? info->cmdline : "");
	json_builder_set_member_name(b, "cwd");
	json_builder_add_string_value(b, info->cwd != NULL ? info->cwd : "");
	json_builder_end_object(b);
	gowl_process_info_free(info);
	return builder_finish(b);
}

static JsonNode *
m_window_type_text(GowlIpc *ipc, const GowlIpcCaller *caller, JsonObject *p,
                   gpointer data, GError **error)
{
	g_autoptr(GowlCompositor) self = comp_get(data, error);
	GowlClient *c;
	gint sent;
	JsonObject *o;
	JsonNode *n;

	(void)ipc; (void)caller;
	if (self == NULL || refuse_locked(self, error))
		return NULL;
	c = param_window(self, p, "id", TRUE, error);
	if (c == NULL)
		return NULL;
	sent = gowl_compositor_send_text_to_client(self, c,
		json_object_get_string_member(p, "text"));
	o = json_object_new();
	json_object_set_int_member(o, "sent", sent);
	n = json_node_new(JSON_NODE_OBJECT);
	json_node_take_object(n, o);
	return n;
}

static JsonNode *
m_window_send_key(GowlIpc *ipc, const GowlIpcCaller *caller, JsonObject *p,
                  gpointer data, GError **error)
{
	g_autoptr(GowlCompositor) self = comp_get(data, error);
	GowlClient *c;
	const gchar *key;
	guint mods = 0, sym = 0;
	guint32 code;
	gboolean shift = FALSE;

	(void)ipc; (void)caller;
	if (self == NULL || refuse_locked(self, error))
		return NULL;
	c = param_window(self, p, "id", TRUE, error);
	if (c == NULL)
		return NULL;
	key = json_object_get_string_member(p, "key");
	if (!gowl_keybind_parse(key, &mods, &sym)) {
		g_set_error(error, GOWL_IPC_ERROR, GOWL_IPC_ERROR_INVALID_PARAMS,
		            "cannot read key '%s' (write it as Ctrl+c, "
		            "Super+Return ...)", key);
		return NULL;
	}
	code = gowl_compositor_keysym_to_keycode(self, sym, &shift);
	if (code == 0) {
		g_set_error(error, GOWL_IPC_ERROR, GOWL_IPC_ERROR_NOT_FOUND,
		            "no key in the current keymap types '%s'", key);
		return NULL;
	}
	if (shift)
		mods |= WLR_MODIFIER_SHIFT;
	if (!gowl_compositor_send_key_to_client(self, c, code, mods)) {
		g_set_error(error, GOWL_IPC_ERROR, GOWL_IPC_ERROR_FAILED,
		            "the key could not be delivered to window %u", c->id);
		return NULL;
	}
	return ok_node();
}

static JsonNode *
screenshot_result(GBytes *rgba, gint w, gint h, const gchar *path,
                  GError **error)
{
	JsonBuilder *b;

	if (!gowl_compositor_save_png(rgba, w, h, path, error))
		return NULL;
	b = json_builder_new();
	json_builder_begin_object(b);
	json_builder_set_member_name(b, "path");
	json_builder_add_string_value(b, path);
	json_builder_set_member_name(b, "width");
	json_builder_add_int_value(b, w);
	json_builder_set_member_name(b, "height");
	json_builder_add_int_value(b, h);
	json_builder_end_object(b);
	return builder_finish(b);
}

static gboolean
check_png_path(const gchar *path, GError **error)
{
	if (!g_path_is_absolute(path)) {
		g_set_error(error, GOWL_IPC_ERROR, GOWL_IPC_ERROR_INVALID_PARAMS,
		            "'path' must be absolute (the compositor's working "
		            "directory is not the caller's): %s", path);
		return FALSE;
	}
	return TRUE;
}

static JsonNode *
m_window_screenshot(GowlIpc *ipc, const GowlIpcCaller *caller,
                    JsonObject *p, gpointer data, GError **error)
{
	g_autoptr(GowlCompositor) self = comp_get(data, error);
	g_autoptr(GBytes) rgba = NULL;
	const gchar *path = json_object_get_string_member(p, "path");
	GowlClient *c;
	gint w = 0, h = 0;

	(void)ipc; (void)caller;
	if (self == NULL || refuse_locked(self, error)
	    || !check_png_path(path, error))
		return NULL;
	c = param_window(self, p, "id", TRUE, error);
	if (c == NULL)
		return NULL;
	rgba = gowl_compositor_screenshot_client(self, c, &w, &h, error);
	if (rgba == NULL)
		return NULL;
	return screenshot_result(rgba, w, h, path, error);
}

/* ── layout.* ────────────────────────────────────────────────────── */

static JsonNode *
m_layout_list(GowlIpc *ipc, const GowlIpcCaller *caller, JsonObject *p,
              gpointer data, GError **error)
{
	g_autoptr(GowlCompositor) self = comp_get(data, error);
	GList *names, *l;
	JsonBuilder *b;

	(void)ipc; (void)caller; (void)p;
	if (self == NULL)
		return NULL;
	names = gowl_layout_list(self);
	b = json_builder_new();
	json_builder_begin_array(b);
	for (l = names; l != NULL; l = l->next) {
		GowlLayoutEntry *e = gowl_layout_lookup(self, (const gchar *)l->data);

		json_builder_begin_object(b);
		json_builder_set_member_name(b, "name");
		json_builder_add_string_value(b, (const gchar *)l->data);
		json_builder_set_member_name(b, "symbol");
		json_builder_add_string_value(b,
			e != NULL && e->symbol != NULL ? e->symbol : "");
		json_builder_end_object(b);
	}
	json_builder_end_array(b);
	g_list_free(names);
	return builder_finish(b);
}

static JsonNode *
layout_of(GowlCompositor *self, GowlMonitor *m)
{
	JsonBuilder *b = json_builder_new();
	GowlLayoutEntry *e = gowl_layout_get(self, m);

	json_builder_begin_object(b);
	json_builder_set_member_name(b, "monitor");
	json_builder_add_string_value(b, mon_name(m));
	json_builder_set_member_name(b, "name");
	json_builder_add_string_value(b, e != NULL && e->name != NULL ? e->name : "");
	json_builder_set_member_name(b, "symbol");
	json_builder_add_string_value(b,
		e != NULL && e->symbol != NULL ? e->symbol : "");
	json_builder_set_member_name(b, "mfact");
	json_builder_add_double_value(b, m->mfact);
	json_builder_set_member_name(b, "nmaster");
	json_builder_add_int_value(b, m->nmaster);
	json_builder_end_object(b);
	return builder_finish(b);
}

static JsonNode *
m_layout_get(GowlIpc *ipc, const GowlIpcCaller *caller, JsonObject *p,
             gpointer data, GError **error)
{
	g_autoptr(GowlCompositor) self = comp_get(data, error);
	GowlMonitor *m;

	(void)ipc; (void)caller;
	if (self == NULL)
		return NULL;
	m = param_monitor(self, p, "monitor", error);
	return m != NULL ? layout_of(self, m) : NULL;
}

static JsonNode *
m_layout_set(GowlIpc *ipc, const GowlIpcCaller *caller, JsonObject *p,
             gpointer data, GError **error)
{
	g_autoptr(GowlCompositor) self = comp_get(data, error);
	const gchar *name = json_object_get_string_member(p, "name");
	GowlMonitor *m;

	(void)ipc; (void)caller;
	if (self == NULL)
		return NULL;
	m = param_monitor(self, p, "monitor", error);
	if (m == NULL)
		return NULL;
	if (!gowl_layout_set(self, m, name)) {
		g_set_error(error, GOWL_IPC_ERROR, GOWL_IPC_ERROR_NOT_FOUND,
		            "no layout '%s' (layout.list names them)", name);
		return NULL;
	}
	return layout_of(self, m);
}

static JsonNode *
m_layout_set_mfact(GowlIpc *ipc, const GowlIpcCaller *caller, JsonObject *p,
                   gpointer data, GError **error)
{
	g_autoptr(GowlCompositor) self = comp_get(data, error);
	GowlMonitor *m;
	gdouble v;

	(void)ipc; (void)caller;
	if (self == NULL)
		return NULL;
	m = param_monitor(self, p, "monitor", error);
	if (m == NULL)
		return NULL;
	v = param_double(p, "value", m->mfact);
	if (json_object_get_boolean_member_with_default(p, "relative", FALSE))
		v = m->mfact + v;
	if (v < 0.05 || v > 0.95) {
		g_set_error(error, GOWL_IPC_ERROR, GOWL_IPC_ERROR_INVALID_PARAMS,
		            "mfact is 0.05 to 0.95, not %g", v);
		return NULL;
	}
	gowl_monitor_set_mfact(m, v);
	gowl_compositor_arrange(self, m);
	return layout_of(self, m);
}

static JsonNode *
m_layout_set_nmaster(GowlIpc *ipc, const GowlIpcCaller *caller, JsonObject *p,
                     gpointer data, GError **error)
{
	g_autoptr(GowlCompositor) self = comp_get(data, error);
	GowlMonitor *m;
	gint64 v;

	(void)ipc; (void)caller;
	if (self == NULL)
		return NULL;
	m = param_monitor(self, p, "monitor", error);
	if (m == NULL)
		return NULL;
	v = param_int(p, "value", m->nmaster);
	if (json_object_get_boolean_member_with_default(p, "relative", FALSE))
		v = m->nmaster + v;
	if (v < 0 || v > 64) {
		g_set_error(error, GOWL_IPC_ERROR, GOWL_IPC_ERROR_INVALID_PARAMS,
		            "nmaster is 0 to 64, not %" G_GINT64_FORMAT, v);
		return NULL;
	}
	gowl_monitor_set_nmaster(m, (gint)v);
	gowl_compositor_arrange(self, m);
	return layout_of(self, m);
}

/* Resolves a JSON array of window ids, every one, or none. */
static GList *
ids_to_clients(GowlCompositor *self, JsonArray *ids, GError **error)
{
	GList *out = NULL;
	guint i;

	for (i = 0; i < json_array_get_length(ids); i++) {
		JsonNode *n = json_array_get_element(ids, i);
		GowlClient *c;

		if (!JSON_NODE_HOLDS_VALUE(n)
		    || json_node_get_value_type(n) != G_TYPE_INT64) {
			g_set_error(error, GOWL_IPC_ERROR,
			            GOWL_IPC_ERROR_INVALID_PARAMS,
			            "element %u is not a window id", i);
			g_list_free(out);
			return NULL;
		}
		c = client_find(self, (guint)json_node_get_int(n));
		if (c == NULL) {
			g_set_error(error, GOWL_IPC_ERROR, GOWL_IPC_ERROR_NOT_FOUND,
			            "no window %" G_GINT64_FORMAT,
			            json_node_get_int(n));
			g_list_free(out);
			return NULL;
		}
		if (refuse_embedded(c, error)) {
			g_list_free(out);
			return NULL;
		}
		out = g_list_append(out, c);
	}
	return out;
}

/*
 * layout.order: the tiling order of some windows.  They keep the slots
 * they already occupy and are dealt back into them in this order, so
 * the first id ends up in whichever of those slots came first -- the
 * master, when one of them holds it.
 */
static JsonNode *
m_layout_order(GowlIpc *ipc, const GowlIpcCaller *caller, JsonObject *p,
               gpointer data, GError **error)
{
	g_autoptr(GowlCompositor) self = comp_get(data, error);
	GList *order;
	gboolean tile = json_object_get_boolean_member_with_default(
		p, "tile", FALSE);
	GList *l;

	(void)ipc; (void)caller;
	if (self == NULL)
		return NULL;
	order = ids_to_clients(self, json_object_get_array_member(p, "ids"),
	                       error);
	if (order == NULL)
		return error != NULL && *error != NULL ? NULL : ok_node();
	/* With tile, floating windows are put back into the layout first,
	 * which is what "make these the tiled stack in this order" means. */
	if (tile)
		for (l = order; l != NULL; l = l->next) {
			GowlClient *c = (GowlClient *)l->data;

			if (c->isfullscreen)
				gowl_compositor_set_client_fullscreen(self, c, FALSE);
			if (c->isfloating)
				gowl_compositor_set_floating(self, c, FALSE);
		}
	gowl_compositor_reorder_clients(self, order);
	g_list_free(order);
	return ok_node();
}

/*
 * layout.apply: place many windows at once.  Every entry is checked
 * before anything moves, so a typo in the fifth entry does not leave
 * four windows moved and the rest where they were.
 */
static JsonNode *
m_layout_apply(GowlIpc *ipc, const GowlIpcCaller *caller, JsonObject *p,
               gpointer data, GError **error)
{
	g_autoptr(GowlCompositor) self = comp_get(data, error);
	JsonArray *wins = NULL;
	const gchar *relative_to = str_member(p, "relative_to", "area");
	const gchar *units = str_member(p, "units", "px");
	GowlMonitor *default_mon = NULL;
	GPtrArray *targets;
	GArray *boxes;
	JsonBuilder *b;
	guint i;

	(void)ipc;
	if (self == NULL)
		return NULL;
	wins = json_object_get_array_member(p, "windows");
	if (json_object_has_member(p, "monitor")) {
		default_mon = param_monitor(self, p, "monitor", error);
		if (default_mon == NULL)
			return NULL;
	}

	targets = g_ptr_array_new();
	boxes = g_array_new(FALSE, FALSE, sizeof(struct wlr_box));

	/* Pass 1: resolve everything. */
	for (i = 0; i < json_array_get_length(wins); i++) {
		JsonNode *en = json_array_get_element(wins, i);
		JsonObject *e;
		GowlClient *c;
		GowlMonitor *m;
		struct wlr_box box;
		const gchar *mname;

		if (!JSON_NODE_HOLDS_OBJECT(en)) {
			g_set_error(error, GOWL_IPC_ERROR,
			            GOWL_IPC_ERROR_INVALID_PARAMS,
			            "windows[%u] is not an object", i);
			goto fail;
		}
		e = json_node_get_object(en);
		{
			/* The entries are not declared parameters, so the
			 * transport cannot check them; a misspelt "widht"
			 * here must still be an error. */
			static const gchar *const ok_keys[] = {
				"id", "x", "y", "width", "height", "monitor", NULL
			};
			JsonObjectIter it;
			const gchar *k;
			JsonNode *v;

			json_object_iter_init(&it, e);
			while (json_object_iter_next(&it, &k, &v)) {
				if (!g_strv_contains(ok_keys, k)) {
					g_set_error(error, GOWL_IPC_ERROR,
					            GOWL_IPC_ERROR_INVALID_PARAMS,
					            "windows[%u] has no member '%s' "
					            "(it takes id, x, y, width, height, "
					            "monitor)", i, k);
					goto fail;
				}
			}
		}
		if (!json_object_has_member(e, "id")
		    || !JSON_NODE_HOLDS_VALUE(json_object_get_member(e, "id"))
		    || (json_node_get_value_type(json_object_get_member(e, "id"))
		            != G_TYPE_INT64
		        && g_strcmp0(json_object_get_string_member(e, "id"),
		                     "focused") != 0)) {
			g_set_error(error, GOWL_IPC_ERROR,
			            GOWL_IPC_ERROR_INVALID_PARAMS,
			            "windows[%u] needs an 'id': a window id or "
			            "\"focused\"", i);
			goto fail;
		}
		{
			/* Coordinates must be numbers here too. */
			static const gchar *const nums[] = {
				"x", "y", "width", "height", NULL
			};
			guint k;

			for (k = 0; nums[k] != NULL; k++) {
				JsonNode *v = json_object_has_member(e, nums[k])
					? json_object_get_member(e, nums[k]) : NULL;

				if (v != NULL && !(JSON_NODE_HOLDS_VALUE(v)
				    && (json_node_get_value_type(v) == G_TYPE_INT64
				        || json_node_get_value_type(v)
				           == G_TYPE_DOUBLE))) {
					g_set_error(error, GOWL_IPC_ERROR,
					            GOWL_IPC_ERROR_INVALID_PARAMS,
					            "windows[%u].%s must be a number",
					            i, nums[k]);
					goto fail;
				}
			}
		}
		c = param_window(self, e, "id", FALSE, error);
		if (c == NULL || refuse_embedded(c, error))
			goto fail;

		/* Which output the coordinates are relative to: the entry's
		 * own, then the request's, then the window's. */
		mname = json_object_get_string_member_with_default(e, "monitor",
		                                                   NULL);
		if (mname != NULL) {
			m = param_monitor(self, e, "monitor", error);
			if (m == NULL)
				goto fail;
		} else {
			m = default_mon != NULL ? default_mon : c->mon;
		}
		if (!resolve_box(e, m, &c->geom, relative_to, units, &box, error)) {
			g_prefix_error(error, "windows[%u]: ", i);
			goto fail;
		}
		g_ptr_array_add(targets, c);
		g_array_append_val(boxes, box);
	}

	/* Pass 2: move them.  A window placed on another output is sent
	 * there first, onto what that output shows. */
	for (i = 0; i < targets->len; i++) {
		GowlClient *c = g_ptr_array_index(targets, i);
		struct wlr_box *box = &g_array_index(boxes, struct wlr_box, i);
		GList *l;
		GowlMonitor *at = NULL;

		for (l = self->monitors; l != NULL && at == NULL; l = l->next) {
			GowlMonitor *m = (GowlMonitor *)l->data;

			if (box->x + box->width / 2 >= m->m.x
			    && box->x + box->width / 2 < m->m.x + m->m.width
			    && box->y + box->height / 2 >= m->m.y
			    && box->y + box->height / 2 < m->m.y + m->m.height)
				at = m;
		}
		if (at != NULL && at != c->mon)
			gowl_compositor_move_client(self, c, at,
			                            at->tagset[at->seltags]);
		place_window(self, c, box);
	}

	b = json_builder_new();
	json_builder_begin_array(b);
	for (i = 0; i < targets->len; i++)
		add_window(self, b, g_ptr_array_index(targets, i), caller);
	json_builder_end_array(b);
	g_ptr_array_unref(targets);
	g_array_unref(boxes);
	return builder_finish(b);

fail:
	g_ptr_array_unref(targets);
	g_array_unref(boxes);
	return NULL;
}

/* ── monitors.* / monitor.* ──────────────────────────────────────── */

static JsonNode *
m_monitors_list(GowlIpc *ipc, const GowlIpcCaller *caller, JsonObject *p,
                gpointer data, GError **error)
{
	g_autoptr(GowlCompositor) self = comp_get(data, error);
	JsonBuilder *b;
	GList *l;

	(void)ipc; (void)caller; (void)p;
	if (self == NULL)
		return NULL;
	b = json_builder_new();
	json_builder_begin_array(b);
	for (l = self->monitors; l != NULL; l = l->next)
		add_monitor(self, b, (GowlMonitor *)l->data);
	json_builder_end_array(b);
	return builder_finish(b);
}

static JsonNode *
m_monitor_get(GowlIpc *ipc, const GowlIpcCaller *caller, JsonObject *p,
              gpointer data, GError **error)
{
	g_autoptr(GowlCompositor) self = comp_get(data, error);
	GowlMonitor *m;

	(void)ipc; (void)caller;
	if (self == NULL)
		return NULL;
	m = param_monitor(self, p, "monitor", error);
	return m != NULL ? monitor_node(self, m) : NULL;
}

static JsonNode *
m_monitor_focus(GowlIpc *ipc, const GowlIpcCaller *caller, JsonObject *p,
                gpointer data, GError **error)
{
	g_autoptr(GowlCompositor) self = comp_get(data, error);
	GowlMonitor *m;

	(void)ipc; (void)caller;
	if (self == NULL || refuse_locked(self, error))
		return NULL;
	m = param_monitor(self, p, "monitor", error);
	if (m == NULL)
		return NULL;
	self->selmon = m;
	gowl_compositor_focus_client(self, focustop_public(self, m), TRUE);
	if (json_object_get_boolean_member_with_default(p, "warp", FALSE))
		gowl_compositor_warp_cursor(self, m->m.x + m->m.width / 2.0,
		                            m->m.y + m->m.height / 2.0);
	return monitor_node(self, m);
}

/*
 * monitor.configure: an output's place, scale, rotation and whether it
 * is on.  Each change is applied in turn and the first refusal stops
 * there, saying which.
 */
static JsonNode *
m_monitor_configure(GowlIpc *ipc, const GowlIpcCaller *caller, JsonObject *p,
                    gpointer data, GError **error)
{
	g_autoptr(GowlCompositor) self = comp_get(data, error);
	GowlMonitor *m;

	(void)ipc; (void)caller;
	if (self == NULL)
		return NULL;
	m = param_monitor(self, p, "monitor", error);
	if (m == NULL)
		return NULL;

	if (json_object_has_member(p, "enabled")
	    && !gowl_monitor_set_enabled(m,
	            json_object_get_boolean_member(p, "enabled"))) {
		g_set_error_literal(error, GOWL_IPC_ERROR, GOWL_IPC_ERROR_REFUSED,
		                    "the output refused to change enabled");
		return NULL;
	}
	if (json_object_has_member(p, "scale")) {
		gdouble s = param_double(p, "scale", 1.0);

		if (s < 0.25 || s > 8.0) {
			g_set_error(error, GOWL_IPC_ERROR,
			            GOWL_IPC_ERROR_INVALID_PARAMS,
			            "scale is 0.25 to 8, not %g", s);
			return NULL;
		}
		if (!gowl_monitor_set_scale(m, s)) {
			g_set_error_literal(error, GOWL_IPC_ERROR,
			                    GOWL_IPC_ERROR_REFUSED,
			                    "the output refused the scale");
			return NULL;
		}
	}
	if (json_object_has_member(p, "transform")) {
		gint64 t = param_int(p, "transform", 0);

		if (t < 0 || t > 7) {
			g_set_error_literal(error, GOWL_IPC_ERROR,
			                    GOWL_IPC_ERROR_INVALID_PARAMS,
			                    "transform is 0-7 (wl_output_transform)");
			return NULL;
		}
		if (!gowl_monitor_set_transform(m, (gint)t)) {
			g_set_error_literal(error, GOWL_IPC_ERROR,
			                    GOWL_IPC_ERROR_REFUSED,
			                    "the output refused the transform");
			return NULL;
		}
	}
	if (json_object_has_member(p, "x") || json_object_has_member(p, "y")) {
		if (!gowl_monitor_set_position(m,
		        (gint)param_int(p, "x", m->m.x),
		        (gint)param_int(p, "y", m->m.y))) {
			g_set_error_literal(error, GOWL_IPC_ERROR,
			                    GOWL_IPC_ERROR_REFUSED,
			                    "the output refused the position");
			return NULL;
		}
	}
	if (json_object_has_member(p, "power"))
		gowl_compositor_set_monitor_powered(self, m,
			json_object_get_boolean_member(p, "power"));
	if (json_object_has_member(p, "hdr")) {
		gboolean on = json_object_get_boolean_member(p, "hdr");

		if (on && !gowl_monitor_supports_hdr(m)) {
			g_set_error_literal(error, GOWL_IPC_ERROR,
			                    GOWL_IPC_ERROR_REFUSED,
			                    "the output cannot do HDR");
			return NULL;
		}
		if (!gowl_monitor_set_hdr(m, on)) {
			g_set_error_literal(error, GOWL_IPC_ERROR,
			                    GOWL_IPC_ERROR_REFUSED,
			                    "the output refused HDR");
			return NULL;
		}
	}
	return monitor_node(self, m);
}

static JsonNode *
m_monitor_screenshot(GowlIpc *ipc, const GowlIpcCaller *caller,
                     JsonObject *p, gpointer data, GError **error)
{
	g_autoptr(GowlCompositor) self = comp_get(data, error);
	g_autoptr(GBytes) rgba = NULL;
	const gchar *path = json_object_get_string_member(p, "path");
	GowlMonitor *m;
	gint w = 0, h = 0;

	(void)ipc; (void)caller;
	if (self == NULL || refuse_locked(self, error)
	    || !check_png_path(path, error))
		return NULL;
	m = param_monitor(self, p, "monitor", error);
	if (m == NULL)
		return NULL;
	rgba = gowl_compositor_screenshot_output(self, mon_name(m), &w, &h, error);
	if (rgba == NULL)
		return NULL;
	return screenshot_result(rgba, w, h, path, error);
}

/* ── tags.* ──────────────────────────────────────────────────────── */

static JsonNode *
tags_of(GowlCompositor *self, GowlMonitor *m)
{
	JsonBuilder *b = json_builder_new();
	guint32 urgent;
	guint32 occ = mon_occupied(self, m, &urgent);

	json_builder_begin_object(b);
	json_builder_set_member_name(b, "monitor");
	json_builder_add_string_value(b, mon_name(m));
	json_builder_set_member_name(b, "active");
	json_builder_add_int_value(b, m->tagset[m->seltags]);
	json_builder_set_member_name(b, "previous");
	json_builder_add_int_value(b, m->tagset[m->seltags ^ 1]);
	json_builder_set_member_name(b, "occupied");
	json_builder_add_int_value(b, occ);
	json_builder_set_member_name(b, "urgent");
	json_builder_add_int_value(b, urgent);
	json_builder_end_object(b);
	return builder_finish(b);
}

static JsonNode *
m_tags_get(GowlIpc *ipc, const GowlIpcCaller *caller, JsonObject *p,
           gpointer data, GError **error)
{
	g_autoptr(GowlCompositor) self = comp_get(data, error);
	GowlMonitor *m;

	(void)ipc; (void)caller;
	if (self == NULL)
		return NULL;
	m = param_monitor(self, p, "monitor", error);
	return m != NULL ? tags_of(self, m) : NULL;
}

static JsonNode *
m_tags_view(GowlIpc *ipc, const GowlIpcCaller *caller, JsonObject *p,
            gpointer data, GError **error)
{
	g_autoptr(GowlCompositor) self = comp_get(data, error);
	GowlMonitor *m;
	guint32 tags;

	(void)ipc; (void)caller;
	if (self == NULL || refuse_locked(self, error))
		return NULL;
	m = param_monitor(self, p, "monitor", error);
	if (m == NULL)
		return NULL;
	tags = param_tags(p, TRUE, error);
	if (tags == 0)
		return NULL;
	gowl_compositor_view_tags(self, m, tags);
	return tags_of(self, m);
}

static JsonNode *
m_tags_toggle(GowlIpc *ipc, const GowlIpcCaller *caller, JsonObject *p,
              gpointer data, GError **error)
{
	g_autoptr(GowlCompositor) self = comp_get(data, error);
	GowlMonitor *m;
	guint32 tags, now;

	(void)ipc; (void)caller;
	if (self == NULL || refuse_locked(self, error))
		return NULL;
	m = param_monitor(self, p, "monitor", error);
	if (m == NULL)
		return NULL;
	tags = param_tags(p, TRUE, error);
	if (tags == 0)
		return NULL;
	now = m->tagset[m->seltags] ^ tags;
	/* Toggling the last tag off would show nothing; dwm refuses, and
	 * so does this. */
	if (now != 0)
		gowl_compositor_view_tags(self, m, now);
	return tags_of(self, m);
}

/* ── pointer.* / keyboard.* / keymode.* / keys ───────────────────── */

static JsonNode *
m_pointer_get(GowlIpc *ipc, const GowlIpcCaller *caller, JsonObject *p,
              gpointer data, GError **error)
{
	g_autoptr(GowlCompositor) self = comp_get(data, error);
	JsonBuilder *b;
	GowlClient *under = NULL;
	gdouble x = 0.0, y = 0.0;

	(void)ipc; (void)caller; (void)p;
	if (self == NULL)
		return NULL;
	if (self->wlr_cursor != NULL) {
		x = self->wlr_cursor->x;
		y = self->wlr_cursor->y;
		under = gowl_compositor_client_at(self, x, y);
	}
	b = json_builder_new();
	json_builder_begin_object(b);
	json_builder_set_member_name(b, "x");
	json_builder_add_double_value(b, x);
	json_builder_set_member_name(b, "y");
	json_builder_add_double_value(b, y);
	json_builder_set_member_name(b, "window");
	if (under != NULL)
		json_builder_add_int_value(b, under->id);
	else
		json_builder_add_null_value(b);
	json_builder_set_member_name(b, "confined");
	json_builder_add_boolean_value(b,
		gowl_compositor_pointer_confine_is_active(self));
	json_builder_end_object(b);
	return builder_finish(b);
}

static JsonNode *
m_pointer_warp(GowlIpc *ipc, const GowlIpcCaller *caller, JsonObject *p,
               gpointer data, GError **error)
{
	g_autoptr(GowlCompositor) self = comp_get(data, error);
	gdouble x, y;

	(void)ipc;
	if (self == NULL || refuse_locked(self, error))
		return NULL;
	x = param_double(p, "x", 0.0);
	y = param_double(p, "y", 0.0);
	/* To a window, if one is named: x/y then count from its corner. */
	if (json_object_has_member(p, "window")) {
		GowlClient *c = param_window(self, p, "window", FALSE, error);

		if (c == NULL)
			return NULL;
		if (!json_object_has_member(p, "x"))
			x = c->geom.width / 2.0;
		if (!json_object_has_member(p, "y"))
			y = c->geom.height / 2.0;
		x += c->geom.x;
		y += c->geom.y;
	}
	gowl_compositor_warp_cursor(self, x, y);
	return m_pointer_get(ipc, caller, p, data, error);
}

static JsonNode *
keyboard_layout_node(GowlCompositor *self)
{
	guint idx = 0;
	const gchar *name = gowl_compositor_get_keyboard_layout(self, &idx);
	JsonObject *o = json_object_new();
	JsonNode *n = json_node_new(JSON_NODE_OBJECT);

	json_object_set_string_member(o, "name", name != NULL ? name : "");
	json_object_set_int_member(o, "index", idx);
	json_node_take_object(n, o);
	return n;
}

static JsonNode *
m_keyboard_get_layout(GowlIpc *ipc, const GowlIpcCaller *caller,
                      JsonObject *p, gpointer data, GError **error)
{
	g_autoptr(GowlCompositor) self = comp_get(data, error);

	(void)ipc; (void)caller; (void)p;
	return self != NULL ? keyboard_layout_node(self) : NULL;
}

static JsonNode *
m_keyboard_set_layout(GowlIpc *ipc, const GowlIpcCaller *caller,
                      JsonObject *p, gpointer data, GError **error)
{
	g_autoptr(GowlCompositor) self = comp_get(data, error);
	JsonNode *n;
	g_autofree gchar *spec = NULL;

	(void)ipc; (void)caller;
	if (self == NULL)
		return NULL;
	n = json_object_get_member(p, "layout");
	/* "next", "prev", or an index. */
	if (JSON_NODE_HOLDS_VALUE(n)
	    && json_node_get_value_type(n) == G_TYPE_INT64)
		spec = g_strdup_printf("%" G_GINT64_FORMAT, json_node_get_int(n));
	else if (JSON_NODE_HOLDS_VALUE(n)
	         && json_node_get_value_type(n) == G_TYPE_STRING)
		spec = g_strdup(json_node_get_string(n));
	else {
		g_set_error_literal(error, GOWL_IPC_ERROR,
		                    GOWL_IPC_ERROR_INVALID_PARAMS,
		                    "layout is \"next\", \"prev\" or an index");
		return NULL;
	}
	gowl_compositor_switch_keyboard_layout(self, spec);
	return keyboard_layout_node(self);
}

static JsonNode *
m_keymode_get(GowlIpc *ipc, const GowlIpcCaller *caller, JsonObject *p,
              gpointer data, GError **error)
{
	g_autoptr(GowlCompositor) self = comp_get(data, error);

	(void)ipc; (void)caller; (void)p;
	return self != NULL ? string_node(gowl_compositor_get_key_mode(self))
	                    : NULL;
}

static JsonNode *
m_keymode_set(GowlIpc *ipc, const GowlIpcCaller *caller, JsonObject *p,
              gpointer data, GError **error)
{
	g_autoptr(GowlCompositor) self = comp_get(data, error);

	(void)ipc; (void)caller;
	if (self == NULL)
		return NULL;
	gowl_compositor_set_key_mode(self, json_object_get_string_member(p, "mode"));
	return string_node(gowl_compositor_get_key_mode(self));
}

static JsonNode *
m_keybinds_list(GowlIpc *ipc, const GowlIpcCaller *caller, JsonObject *p,
                gpointer data, GError **error)
{
	g_autoptr(GowlCompositor) self = comp_get(data, error);
	g_autofree gchar *text = NULL;
	g_autoptr(JsonParser) parser = NULL;

	(void)ipc; (void)caller; (void)p;
	if (self == NULL)
		return NULL;
	/* One definition of what a keybind looks like as JSON: the text
	 * `keybinds' query's. */
	text = gowl_compositor_ipc_command(self, "keybinds");
	parser = json_parser_new_immutable();
	if (text == NULL || !json_parser_load_from_data(parser, text, -1, error))
		return NULL;
	return json_node_copy(json_parser_get_root(parser));
}

static JsonNode *
m_key_dispatch(GowlIpc *ipc, const GowlIpcCaller *caller, JsonObject *p,
               gpointer data, GError **error)
{
	g_autoptr(GowlCompositor) self = comp_get(data, error);
	const gchar *key = json_object_get_string_member(p, "key");
	guint mods, sym;

	(void)ipc; (void)caller;
	if (self == NULL)
		return NULL;
	if (!gowl_keybind_parse(key, &mods, &sym)) {
		g_set_error(error, GOWL_IPC_ERROR, GOWL_IPC_ERROR_INVALID_PARAMS,
		            "cannot read key '%s'", key);
		return NULL;
	}
	return bool_node(gowl_compositor_dispatch_keybind(self, mods, sym));
}

static JsonNode *
m_action_run(GowlIpc *ipc, const GowlIpcCaller *caller, JsonObject *p,
             gpointer data, GError **error)
{
	g_autoptr(GowlCompositor) self = comp_get(data, error);
	g_autofree gchar *norm = NULL;
	g_autofree gchar *arg = NULL;
	GEnumClass *klass;
	GEnumValue *val;
	GowlKeybindEntry kb;

	(void)ipc; (void)caller;
	if (self == NULL)
		return NULL;
	norm = g_strdup(json_object_get_string_member(p, "name"));
	g_strdelimit(norm, "_", '-');
	klass = (GEnumClass *)g_type_class_ref(GOWL_TYPE_ACTION);
	val = g_enum_get_value_by_nick(klass, norm);
	g_type_class_unref(klass);
	if (val == NULL) {
		g_set_error(error, GOWL_IPC_ERROR, GOWL_IPC_ERROR_NOT_FOUND,
		            "no action '%s' (keybinds.list shows the names in use)",
		            json_object_get_string_member(p, "name"));
		return NULL;
	}
	arg = g_strdup(json_object_get_string_member_with_default(p, "arg", NULL));
	memset(&kb, 0, sizeof kb);
	kb.action = val->value;
	kb.arg = arg;
	return bool_node(gowl_compositor_run_keybind_entry(self, &kb));
}

/* ── spawn ───────────────────────────────────────────────────────── */

/*
 * spawn: start a program, and say where its first window goes.
 *
 * The placement is keyed by the pid the compositor started, which is
 * why the program is started here rather than by the caller: a window
 * is matched to a pid when it maps.  A program that hands its window to
 * an already running instance (a browser's remote, most flatpaks) maps
 * it under a different pid and the placement does not apply; watch
 * `window.added' and place it then.
 */
static JsonNode *
m_spawn(GowlIpc *ipc, const GowlIpcCaller *caller, JsonObject *p,
        gpointer data, GError **error)
{
	g_autoptr(GowlCompositor) self = comp_get(data, error);
	g_auto(GStrv) argv = NULL;
	g_auto(GStrv) envp = NULL;
	const gchar *cwd = json_object_get_string_member_with_default(
		p, "cwd", NULL);
	GPid pid = 0;
	GowlMonitor *m = NULL;
	guint32 tags;
	JsonObject *o;
	JsonNode *n;

	(void)ipc; (void)caller;
	if (self == NULL || refuse_locked(self, error))
		return NULL;

	/* argv: an array, or a command line split the way a shell would
	 * (no shell runs unless `shell' says so). */
	if (json_object_has_member(p, "argv")) {
		JsonArray *a = json_object_get_array_member(p, "argv");
		guint i, len = json_array_get_length(a);

		if (len == 0) {
			g_set_error_literal(error, GOWL_IPC_ERROR,
			                    GOWL_IPC_ERROR_INVALID_PARAMS,
			                    "argv is empty");
			return NULL;
		}
		argv = g_new0(gchar *, len + 1);
		for (i = 0; i < len; i++) {
			JsonNode *e = json_array_get_element(a, i);

			if (!JSON_NODE_HOLDS_VALUE(e)
			    || json_node_get_value_type(e) != G_TYPE_STRING) {
				g_set_error(error, GOWL_IPC_ERROR,
				            GOWL_IPC_ERROR_INVALID_PARAMS,
				            "argv[%u] is not a string", i);
				return NULL;
			}
			argv[i] = g_strdup(json_node_get_string(e));
		}
	} else if (json_object_has_member(p, "command")) {
		const gchar *cmd = json_object_get_string_member(p, "command");

		if (json_object_get_boolean_member_with_default(p, "shell", FALSE)) {
			argv = g_new0(gchar *, 4);
			argv[0] = g_strdup("/bin/sh");
			argv[1] = g_strdup("-c");
			argv[2] = g_strdup(cmd);
		} else if (!g_shell_parse_argv(cmd, NULL, &argv, error)) {
			return NULL;
		}
	} else {
		g_set_error_literal(error, GOWL_IPC_ERROR,
		                    GOWL_IPC_ERROR_INVALID_PARAMS,
		                    "spawn needs 'argv' or 'command'");
		return NULL;
	}

	/* env: added to (or replacing) the compositor's environment. */
	envp = g_get_environ();
	if (json_object_has_member(p, "env")) {
		JsonObject *env = json_object_get_object_member(p, "env");
		JsonObjectIter iter;
		const gchar *k;
		JsonNode *v;

		json_object_iter_init(&iter, env);
		while (json_object_iter_next(&iter, &k, &v)) {
			if (JSON_NODE_HOLDS_NULL(v))
				envp = g_environ_unsetenv(envp, k);
			else if (JSON_NODE_HOLDS_VALUE(v)
			         && json_node_get_value_type(v) == G_TYPE_STRING)
				envp = g_environ_setenv(envp, k,
				                        json_node_get_string(v), TRUE);
			else {
				g_set_error(error, GOWL_IPC_ERROR,
				            GOWL_IPC_ERROR_INVALID_PARAMS,
				            "env.%s must be a string or null", k);
				return NULL;
			}
		}
	}

	if (json_object_has_member(p, "monitor")) {
		m = param_monitor(self, p, "monitor", error);
		if (m == NULL)
			return NULL;
	}
	tags = param_tags(p, FALSE, error);
	if (error != NULL && *error != NULL)
		return NULL;

	/* Without DO_NOT_REAP_CHILD GLib double-forks and reports the
	 * grandchild's pid -- the one whose window will map -- and nothing
	 * needs to wait on it, which matters because nothing here may
	 * block and no GLib main loop is promised to be turning. */
	if (!g_spawn_async(cwd, argv, envp, G_SPAWN_SEARCH_PATH, NULL, NULL,
	                   &pid, error))
		return NULL;

	if (tags != 0 || m != NULL) {
		gint idx = -1;

		if (m != NULL)
			idx = g_list_index(self->monitors, m);
		if (tags == 0 && m != NULL)
			tags = m->tagset[m->seltags];
		gowl_compositor_pretag_pid(self, (pid_t)pid, tags, idx);
	}
	if (json_object_get_boolean_member_with_default(p, "floating", FALSE)
	    || json_object_has_member(p, "width")
	    || json_object_has_member(p, "height")) {
		GowlMonitor *at = m != NULL ? m : self->selmon;
		struct wlr_box cur = { 0, 0, 800, 600 };
		struct wlr_box box;

		if (at != NULL) {
			cur.x = at->w.x + (at->w.width - cur.width) / 2;
			cur.y = at->w.y + (at->w.height - cur.height) / 2;
		}
		if (!resolve_box(p, at, &cur, str_member(p, "relative_to", "area"),
		                 str_member(p, "units", "px"), &box, error))
			return NULL;
		gowl_compositor_prefloat_pid_with_hint(self, (pid_t)pid,
			box.x, box.y, box.width, box.height,
			GOWL_SCENE_LAYER_FLOAT, NULL, NULL);
	}

	o = json_object_new();
	json_object_set_int_member(o, "pid", pid);
	n = json_node_new(JSON_NODE_OBJECT);
	json_node_take_object(n, o);
	return n;
}

/* ── transition.* ────────────────────────────────────────────────── */

static JsonNode *
transition_state(GowlCompositor *self)
{
	g_auto(GStrv) names = gowl_compositor_list_tag_transitions(self);
	const gchar *cur = gowl_compositor_get_tag_transition(self);
	JsonBuilder *b = json_builder_new();
	guint i;

	json_builder_begin_object(b);
	json_builder_set_member_name(b, "current");
	json_builder_add_string_value(b, cur);
	json_builder_set_member_name(b, "label");
	json_builder_add_string_value(b,
		gowl_compositor_tag_transition_label(self, cur));
	json_builder_set_member_name(b, "duration");
	json_builder_add_int_value(b, self->config != NULL
		? gowl_config_get_tag_transition_duration(self->config) : 0);
	json_builder_set_member_name(b, "transitions");
	json_builder_begin_array(b);
	for (i = 0; names != NULL && names[i] != NULL; i++) {
		json_builder_begin_object(b);
		json_builder_set_member_name(b, "name");
		json_builder_add_string_value(b, names[i]);
		json_builder_set_member_name(b, "label");
		json_builder_add_string_value(b,
			gowl_compositor_tag_transition_label(self, names[i]));
		json_builder_end_object(b);
	}
	json_builder_end_array(b);
	json_builder_end_object(b);
	return builder_finish(b);
}

static JsonNode *
m_transition_get(GowlIpc *ipc, const GowlIpcCaller *caller, JsonObject *p,
                 gpointer data, GError **error)
{
	g_autoptr(GowlCompositor) self = comp_get(data, error);

	(void)ipc; (void)caller; (void)p;
	return self != NULL ? transition_state(self) : NULL;
}

static JsonNode *
m_transition_set(GowlIpc *ipc, const GowlIpcCaller *caller, JsonObject *p,
                 gpointer data, GError **error)
{
	g_autoptr(GowlCompositor) self = comp_get(data, error);
	const gchar *name = json_object_get_string_member(p, "name");

	(void)ipc; (void)caller;
	if (self == NULL)
		return NULL;
	if (json_object_has_member(p, "duration")) {
		gint64 ms = param_int(p, "duration", 0);

		if (ms < 0 || ms > 10000) {
			g_set_error(error, GOWL_IPC_ERROR,
			            GOWL_IPC_ERROR_INVALID_PARAMS,
			            "duration is 0 to 10000 ms");
			return NULL;
		}
		if (self->config != NULL)
			gowl_config_set_tag_transition_duration(self->config,
			                                        (gint)ms);
	}
	if (!gowl_compositor_set_tag_transition(self, name)) {
		g_set_error(error, GOWL_IPC_ERROR, GOWL_IPC_ERROR_NOT_FOUND,
		            "no transition '%s' is loaded (transition.get lists "
		            "them)", name);
		return NULL;
	}
	return transition_state(self);
}

/* ── session.* ───────────────────────────────────────────────────── */

static JsonNode *
m_session_lock(GowlIpc *ipc, const GowlIpcCaller *caller, JsonObject *p,
               gpointer data, GError **error)
{
	g_autoptr(GowlCompositor) self = comp_get(data, error);

	(void)ipc; (void)caller; (void)p;
	if (self == NULL)
		return NULL;
	gowl_compositor_lock_session(self);
	return ok_node();
}

static JsonNode *
m_session_locked(GowlIpc *ipc, const GowlIpcCaller *caller, JsonObject *p,
                 gpointer data, GError **error)
{
	g_autoptr(GowlCompositor) self = comp_get(data, error);

	(void)ipc; (void)caller; (void)p;
	return self != NULL ? bool_node(gowl_compositor_is_locked(self)) : NULL;
}

static JsonNode *
m_outputs_power(GowlIpc *ipc, const GowlIpcCaller *caller, JsonObject *p,
                gpointer data, GError **error)
{
	g_autoptr(GowlCompositor) self = comp_get(data, error);
	gboolean on;

	(void)ipc; (void)caller;
	if (self == NULL)
		return NULL;
	on = param_toggle(p, "value", !gowl_compositor_any_output_powered_off(self));
	gowl_compositor_set_outputs_powered(self, on);
	return bool_node(on);
}

/* ── Events ──────────────────────────────────────────────────────── */

static JsonNode *
obj_node(JsonObject *o)
{
	JsonNode *n = json_node_new(JSON_NODE_OBJECT);

	json_node_take_object(n, o);
	return n;
}

/*
 * The text events, structured.  Where the text form already carries
 * everything this repeats it typed; where it carries an id, the object
 * it names is attached, so a subscriber reacting to `window.added' does
 * not need a second round trip to learn what was added.
 */
static JsonNode *
format_event(GowlIpc *ipc, const gchar *word, const gchar *args,
             gchar **out_name, gpointer data)
{
	GowlIpcMethodsState *st = (GowlIpcMethodsState *)data;
	g_autoptr(GowlCompositor) self =
		(GowlCompositor *)g_weak_ref_get(&st->compositor);
	g_auto(GStrv) f = g_strsplit(args != NULL ? args : "", " ", 0);
	guint nf = g_strv_length(f);
	JsonObject *o = json_object_new();

	(void)ipc;

	if (self == NULL) {
		json_object_unref(o);
		return NULL;
	}

	if (g_strcmp0(word, "client-added") == 0 && nf >= 1) {
		GowlClient *c = client_find(self, (guint)atoi(f[0]));

		*out_name = g_strdup("window.added");
		json_object_set_int_member(o, "id", atoi(f[0]));
		if (c != NULL)
			json_object_set_member(o, "window",
			                       window_node(self, c, NULL));
	} else if (g_strcmp0(word, "client-removed") == 0 && nf >= 1) {
		*out_name = g_strdup("window.removed");
		json_object_set_int_member(o, "id", atoi(f[0]));
		g_hash_table_remove(st->windows, GUINT_TO_POINTER((guint)atoi(f[0])));
	} else if (g_strcmp0(word, "client-minimized") == 0 && nf >= 1) {
		*out_name = g_strdup("window.minimized");
		json_object_set_int_member(o, "id", atoi(f[0]));
	} else if (g_strcmp0(word, "client-restored") == 0 && nf >= 1) {
		*out_name = g_strdup("window.restored");
		json_object_set_int_member(o, "id", atoi(f[0]));
	} else if (g_strcmp0(word, "focus") == 0) {
		guint id = nf >= 1 ? (guint)atoi(f[0]) : 0;
		GowlClient *c = id != 0 ? client_find(self, id) : NULL;

		*out_name = g_strdup("window.focus");
		if (c != NULL) {
			json_object_set_int_member(o, "id", id);
			json_object_set_member(o, "window",
			                       window_node(self, c, NULL));
		} else {
			json_object_set_null_member(o, "id");
		}
	} else if (g_strcmp0(word, "title") == 0) {
		GowlClient *c = gowl_compositor_get_focused_client(self);

		*out_name = g_strdup("window.title");
		if (c != NULL)
			json_object_set_int_member(o, "id", c->id);
		else
			json_object_set_null_member(o, "id");
		json_object_set_string_member(o, "title", args != NULL ? args : "");
	} else if (g_strcmp0(word, "tags") == 0
	           || g_strcmp0(word, "layout") == 0) {
		/* Produced from the monitor diff instead, which sees every
		 * change rather than only the keybind paths that push these. */
		json_object_unref(o);
		*out_name = g_strdup("");
		return NULL;
	} else if (g_strcmp0(word, "mode") == 0) {
		*out_name = g_strdup("keymode.changed");
		json_object_set_string_member(o, "mode", args != NULL ? args : "");
	} else if (g_strcmp0(word, "keymap") == 0 && nf >= 1) {
		*out_name = g_strdup("keyboard.layout");
		json_object_set_int_member(o, "index", atoi(f[0]));
		json_object_set_string_member(o, "name",
			args != NULL && strchr(args, ' ') != NULL
			? strchr(args, ' ') + 1 : "");
	} else if (g_strcmp0(word, "power") == 0 && nf >= 2) {
		*out_name = g_strdup("monitor.power");
		json_object_set_string_member(o, "monitor", f[0]);
		json_object_set_boolean_member(o, "on", g_strcmp0(f[1], "on") == 0);
	} else if (g_strcmp0(word, "profile") == 0) {
		*out_name = g_strdup("profile.changed");
		if (args != NULL && *args != '\0')
			json_object_set_string_member(o, "profile", args);
		else
			json_object_set_null_member(o, "profile");
	} else if (g_strcmp0(word, "monitor-added") == 0 && nf >= 1) {
		GowlMonitor *m = NULL;
		GList *l;

		*out_name = g_strdup("monitor.added");
		json_object_set_string_member(o, "name", f[0]);
		for (l = self->monitors; l != NULL && m == NULL; l = l->next)
			if (g_strcmp0(mon_name((GowlMonitor *)l->data), f[0]) == 0)
				m = (GowlMonitor *)l->data;
		if (m != NULL)
			json_object_set_member(o, "monitor", monitor_node(self, m));
	} else if (g_strcmp0(word, "monitor-removed") == 0 && nf >= 1) {
		*out_name = g_strdup("monitor.removed");
		json_object_set_string_member(o, "name", f[0]);
		g_hash_table_remove(st->monitors, f[0]);
	} else if (g_strcmp0(word, "lock") == 0) {
		*out_name = g_strdup("session.lock");
		json_object_set_boolean_member(o, "locked",
			g_strcmp0(args, "locked") == 0);
	} else if (g_strcmp0(word, "pointer-confine") == 0 && nf >= 1) {
		gboolean on = g_strcmp0(f[0], "on") == 0;

		*out_name = g_strdup("pointer.confine");
		json_object_set_boolean_member(o, "active", on);
		if (on && nf >= 2)
			json_object_set_int_member(o, "id", atoi(f[1]));
		json_object_set_string_member(o, "reason",
			nf >= (on ? 3u : 2u) ? f[on ? 2 : 1] : "");
	} else if (g_strcmp0(word, "crt") == 0) {
		*out_name = g_strdup("crt.changed");
		json_object_set_boolean_member(o, "on", g_strcmp0(args, "on") == 0);
	} else if (g_strcmp0(word, "transition") == 0) {
		*out_name = g_strdup("transition.changed");
		json_object_set_string_member(o, "name", args != NULL ? args : "");
		json_object_set_string_member(o, "label",
			gowl_compositor_tag_transition_label(self, args));
	} else if (g_strcmp0(word, "backdrop") == 0) {
		*out_name = g_strdup("backdrop.changed");
		json_object_set_string_member(o, "style", args != NULL ? args : "");
	} else if (g_strcmp0(word, "command") == 0) {
		*out_name = g_strdup("command");
		json_object_set_string_member(o, "line", args != NULL ? args : "");
	} else {
		/* A module's event (`macro', `broadcast', ...): its own name,
		 * and the words it sent. */
		JsonArray *a = json_array_new();
		guint i;

		for (i = 0; i < nf; i++)
			if (*f[i] != '\0')
				json_array_add_string_element(a, f[i]);
		json_object_set_string_member(o, "args", args != NULL ? args : "");
		json_object_set_array_member(o, "words", a);
	}
	return obj_node(o);
}

/*
 * window.changed: what moved or changed state since it was last
 * announced.  Computed only when a subscriber wants it, after an
 * arrange and after each frame -- a floating window dragged with the
 * mouse is never arranged, but it is drawn.
 */
static void
diff_windows(GowlIpc *ipc, GowlIpcMethodsState *st, GowlCompositor *self)
{
	GHashTable *seen;
	GHashTableIter iter;
	gpointer key;
	GList *l;
	/* The snapshot is always kept; the event is only built for
	 * somebody. */
	gboolean want = gowl_ipc_wants_event(ipc, "window.changed");

	seen = g_hash_table_new(g_direct_hash, g_direct_equal);
	for (l = self->clients; l != NULL; l = l->next) {
		GowlClient *c = (GowlClient *)l->data;
		GowlIpcWinSnap now;
		GowlIpcWinSnap *was;
		JsonArray *changed;
		JsonObject *o;

		g_hash_table_add(seen, GUINT_TO_POINTER(c->id));
		now.x = c->geom.x;
		now.y = c->geom.y;
		now.width = c->geom.width;
		now.height = c->geom.height;
		now.tags = c->isminimized ? c->minimized_tags : c->tags;
		now.monitor = (gchar *)mon_name(c->mon);
		now.floating = c->isfloating;
		now.fullscreen = c->isfullscreen;
		now.minimized = c->isminimized;
		now.sticky = c->issticky;
		now.below = c->isbelow;
		now.urgent = c->isurgent;
		now.visible = client_visible(c);

		was = g_hash_table_lookup(st->windows, GUINT_TO_POINTER(c->id));
		if (was == NULL) {
			/* First sight: record it; `window.added' announced
			 * it already. */
			was = g_new0(GowlIpcWinSnap, 1);
			*was = now;
			was->monitor = g_strdup(now.monitor);
			g_hash_table_insert(st->windows, GUINT_TO_POINTER(c->id), was);
			continue;
		}

		changed = json_array_new();
		if (was->x != now.x || was->y != now.y
		    || was->width != now.width || was->height != now.height)
			json_array_add_string_element(changed, "geometry");
		if (was->tags != now.tags)
			json_array_add_string_element(changed, "tags");
		if (g_strcmp0(was->monitor, now.monitor) != 0)
			json_array_add_string_element(changed, "monitor");
		if (was->floating != now.floating)
			json_array_add_string_element(changed, "floating");
		if (was->fullscreen != now.fullscreen)
			json_array_add_string_element(changed, "fullscreen");
		if (was->minimized != now.minimized)
			json_array_add_string_element(changed, "minimized");
		if (was->sticky != now.sticky)
			json_array_add_string_element(changed, "sticky");
		if (was->below != now.below)
			json_array_add_string_element(changed, "below");
		if (was->urgent != now.urgent)
			json_array_add_string_element(changed, "urgent");
		if (was->visible != now.visible)
			json_array_add_string_element(changed, "visible");

		if (json_array_get_length(changed) == 0) {
			json_array_unref(changed);
			continue;
		}
		g_free(was->monitor);
		*was = now;
		was->monitor = g_strdup(now.monitor);
		if (!want) {
			json_array_unref(changed);
			continue;
		}

		o = json_object_new();
		json_object_set_int_member(o, "id", c->id);
		json_object_set_array_member(o, "changed", changed);
		json_object_set_member(o, "window", window_node(self, c, NULL));
		gowl_ipc_push_json_event(ipc, "window.changed", obj_node(o));
	}

	/* Forget windows that went away without our hearing it. */
	g_hash_table_iter_init(&iter, st->windows);
	while (g_hash_table_iter_next(&iter, &key, NULL))
		if (!g_hash_table_contains(seen, key))
			g_hash_table_iter_remove(&iter);
	g_hash_table_unref(seen);
}

/*
 * tags.changed / layout.changed / monitor.changed for one output, from
 * a diff against what was last announced.  Run on every arrange, which
 * every one of those changes ends in, whichever path made it.
 */
static void
diff_monitor(GowlIpc *ipc, GowlIpcMethodsState *st, GowlCompositor *self,
             GowlMonitor *m)
{
	GowlLayoutEntry *e = gowl_layout_get(self, m);
	const gchar *name = mon_name(m);
	const gchar *layout = e != NULL && e->name != NULL ? e->name : "";
	GowlIpcMonSnap *was;
	gboolean fresh = FALSE;
	gboolean tags_ch, layout_ch, mon_ch;

	/* The snapshot is kept current whether or not anybody listens: a
	 * subscriber's first change must be compared against the state
	 * just before it, not against whenever the last listener left --
	 * and an idle output draws no frames to catch up on. */
	if (*name == '\0')
		return;

	was = g_hash_table_lookup(st->monitors, name);
	if (was == NULL) {
		was = g_new0(GowlIpcMonSnap, 1);
		g_hash_table_insert(st->monitors, g_strdup(name), was);
		fresh = TRUE;
	}

	tags_ch = !fresh && was->tags != m->tagset[m->seltags];
	layout_ch = !fresh && g_strcmp0(was->layout, layout) != 0;
	mon_ch = !fresh
	         && (was->mfact != m->mfact || was->nmaster != m->nmaster
	             || was->x != m->m.x || was->y != m->m.y
	             || was->width != m->m.width || was->height != m->m.height
	             || was->wx != m->w.x || was->wy != m->w.y
	             || was->wwidth != m->w.width || was->wheight != m->w.height
	             || (m->wlr_output != NULL
	                 && (was->scale != m->wlr_output->scale
	                     || was->transform != (gint)m->wlr_output->transform
	                     || was->enabled != m->wlr_output->enabled)));

	if (tags_ch) {
		JsonObject *o = json_object_new();
		guint32 urgent;
		guint32 occ = mon_occupied(self, m, &urgent);

		json_object_set_string_member(o, "monitor", name);
		json_object_set_int_member(o, "active", m->tagset[m->seltags]);
		json_object_set_int_member(o, "previous", was->tags);
		json_object_set_int_member(o, "occupied", occ);
		json_object_set_int_member(o, "urgent", urgent);
		gowl_ipc_push_json_event(ipc, "tags.changed", obj_node(o));
	}
	if (layout_ch) {
		JsonObject *o = json_object_new();

		json_object_set_string_member(o, "monitor", name);
		json_object_set_string_member(o, "layout", layout);
		json_object_set_string_member(o, "symbol",
			e != NULL && e->symbol != NULL ? e->symbol : "");
		json_object_set_string_member(o, "previous",
			was->layout != NULL ? was->layout : "");
		gowl_ipc_push_json_event(ipc, "layout.changed", obj_node(o));
	}
	if (mon_ch) {
		JsonObject *o = json_object_new();

		json_object_set_string_member(o, "name", name);
		json_object_set_member(o, "monitor", monitor_node(self, m));
		gowl_ipc_push_json_event(ipc, "monitor.changed", obj_node(o));
	}

	was->tags = m->tagset[m->seltags];
	g_free(was->layout);
	was->layout = g_strdup(layout);
	was->mfact = m->mfact;
	was->nmaster = m->nmaster;
	was->x = m->m.x;
	was->y = m->m.y;
	was->width = m->m.width;
	was->height = m->m.height;
	was->wx = m->w.x;
	was->wy = m->w.y;
	was->wwidth = m->w.width;
	was->wheight = m->w.height;
	if (m->wlr_output != NULL) {
		was->scale = m->wlr_output->scale;
		was->transform = (gint)m->wlr_output->transform;
		was->enabled = m->wlr_output->enabled;
	}
}

static void
on_arranged(GowlCompositor *self, GowlMonitor *m, gpointer data)
{
	GowlIpc *ipc = GOWL_IPC(data);
	GowlIpcMethodsState *st = g_object_get_data(G_OBJECT(ipc),
	                                            GOWL_IPC_METHODS_KEY);

	if (st == NULL)
		return;
	if (m != NULL)
		diff_monitor(ipc, st, self, m);
	diff_windows(ipc, st, self);
}

/* Per frame only when somebody listens: a dragged floating window is
 * drawn but never arranged.  Without a listener the arrange-time diff
 * keeps the snapshot close enough. */
static void
on_frame_rendered(GowlCompositor *self, GObject *monitor, gpointer data)
{
	GowlIpc *ipc = GOWL_IPC(data);
	GowlIpcMethodsState *st = g_object_get_data(G_OBJECT(ipc),
	                                            GOWL_IPC_METHODS_KEY);

	(void)monitor;
	if (st != NULL && gowl_ipc_wants_event(ipc, "window.changed"))
		diff_windows(ipc, st, self);
}

static void
declare_events(GowlIpc *ipc)
{
	static const struct { const gchar *name, *doc; } ev[] = {
		{ "window.added",     "a window mapped: {id, window}" },
		{ "window.removed",   "a window unmapped: {id}" },
		{ "window.focus",     "keyboard focus moved: {id|null, window}" },
		{ "window.title",     "the focused window's title: {id, title}" },
		{ "window.changed",   "geometry or state changed: {id, changed[], "
		                      "window}; changed names geometry, tags, "
		                      "monitor, floating, fullscreen, minimized, "
		                      "sticky, below, urgent, visible" },
		{ "window.minimized", "a window was minimized: {id}" },
		{ "window.restored",  "a minimized window came back: {id}" },
		{ "tags.changed",     "an output views other tags, from any source: "
		                      "{monitor, active, previous, occupied, urgent}" },
		{ "layout.changed",   "an output's layout changed: {monitor, layout, "
		                      "symbol, previous}" },
		{ "monitor.changed",  "an output's geometry, window area, scale, "
		                      "transform, mfact or nmaster changed: {name, "
		                      "monitor}" },
		{ "monitor.added",    "an output appeared: {name, monitor}" },
		{ "monitor.removed",  "an output went: {name}" },
		{ "monitor.power",    "an output powered on or off: {monitor, on}" },
		{ "keymode.changed",  "a key mode was entered or left: {mode}" },
		{ "keyboard.layout",  "the keyboard layout changed: {index, name}" },
		{ "profile.changed",  "a different output profile matches: "
		                      "{profile|null}" },
		{ "session.lock",     "the session locked or unlocked: {locked}" },
		{ "pointer.confine",  "pointer confinement changed: {active, id?, "
		                      "reason}" },
		{ "crt.changed",      "the CRT filter switched: {on}" },
		{ "backdrop.changed", "the translucency backdrop changed: {style}" },
		{ "transition.changed", "the tag transition was selected: "
		                        "{name, label}" },
		{ "command",          "a module command ran, from any source: {line}" },
		{ "<module>",         "a module's own event (macro, broadcast, "
		                      "inputremap, screenshot, ...): {args, words[]}" },
		{ NULL, NULL }
	};
	guint i;

	for (i = 0; ev[i].name != NULL; i++)
		gowl_ipc_declare_event(ipc, ev[i].name, ev[i].doc);
}

/* ── Registration ────────────────────────────────────────────────── */

#define P_END { NULL, 0, FALSE, NULL }
#define P_WIN(req) { "id", GOWL_IPC_PARAM_WINDOW, (req), \
	"window id, or \"focused\"" }
#define P_WIN_DEF { "id", GOWL_IPC_PARAM_WINDOW, FALSE, \
	"window id, or \"focused\" (the default)" }
#define P_MON { "monitor", GOWL_IPC_PARAM_MONITOR, FALSE, \
	"output name, or \"focused\" (the default)" }
#define P_TOGGLE { "value", GOWL_IPC_PARAM_TOGGLE, FALSE, \
	"true, false, or \"toggle\" (the default)" }
#define P_TAGS { "tags", GOWL_IPC_PARAM_INT, FALSE, \
	"tag bitmask: tag 1 is 1, tag 3 is 4" }
#define P_TAG { "tag", GOWL_IPC_PARAM_INT, FALSE, \
	"one tag, counted from 1 (instead of tags)" }
#define P_BOX \
	{ "x", GOWL_IPC_PARAM_NUMBER, FALSE, "left edge (unchanged if absent)" }, \
	{ "y", GOWL_IPC_PARAM_NUMBER, FALSE, "top edge" }, \
	{ "width", GOWL_IPC_PARAM_NUMBER, FALSE, "width, border included" }, \
	{ "height", GOWL_IPC_PARAM_NUMBER, FALSE, "height, border included" }, \
	{ "relative_to", GOWL_IPC_PARAM_STRING, FALSE, \
	  "\"layout\" (default here), \"monitor\" or \"area\" (the output " \
	  "less its bars)" }, \
	{ "units", GOWL_IPC_PARAM_STRING, FALSE, \
	  "\"px\" (default) or \"fraction\" of the monitor/area box" }

static const GowlIpcParam p_none[] = { P_END };
static const GowlIpcParam p_win_def[] = { P_WIN_DEF, P_END };
static const GowlIpcParam p_win_req[] = { P_WIN(TRUE), P_END };
static const GowlIpcParam p_mon[] = { P_MON, P_END };
static const GowlIpcParam p_flag[] = { P_WIN_DEF, P_TOGGLE, P_END };
static const GowlIpcParam p_minimize[] = {
	P_WIN_DEF, P_TOGGLE, P_MON, P_TAGS, P_TAG, P_END
};
static const GowlIpcParam p_command[] = {
	{ "line", GOWL_IPC_PARAM_STRING, TRUE,
	  "a text command, as `gowl-msg' takes it (\"expo\", \"view 4\")" },
	P_END
};
static const GowlIpcParam p_windows_list[] = {
	{ "app_id", GOWL_IPC_PARAM_STRING, FALSE, "exactly this app id" },
	{ "title", GOWL_IPC_PARAM_STRING, FALSE, "title contains this" },
	{ "pid", GOWL_IPC_PARAM_INT, FALSE, "exactly this process" },
	{ "descendants_of", GOWL_IPC_PARAM_INT, FALSE,
	  "this process or anything it started" },
	{ "mine", GOWL_IPC_PARAM_BOOL, FALSE,
	  "the calling process's windows and its children's" },
	P_MON, P_TAGS, P_TAG,
	{ "visible", GOWL_IPC_PARAM_BOOL, FALSE, "only windows on screen" },
	{ "floating", GOWL_IPC_PARAM_BOOL, FALSE, "only floating, or only tiled" },
	{ "embedded", GOWL_IPC_PARAM_BOOL, FALSE,
	  "include windows embedded in Emacs buffers (default true)" },
	P_END
};
static const GowlIpcParam p_set_geometry[] = { P_WIN_DEF, P_BOX, P_END };
static const GowlIpcParam p_move_by[] = {
	P_WIN_DEF,
	{ "dx", GOWL_IPC_PARAM_INT, FALSE, "pixels right" },
	{ "dy", GOWL_IPC_PARAM_INT, FALSE, "pixels down" },
	{ "dwidth", GOWL_IPC_PARAM_INT, FALSE, "pixels wider" },
	{ "dheight", GOWL_IPC_PARAM_INT, FALSE, "pixels taller" },
	P_END
};
static const GowlIpcParam p_center[] = {
	P_WIN_DEF,
	{ "width", GOWL_IPC_PARAM_INT, FALSE, "new width (else unchanged)" },
	{ "height", GOWL_IPC_PARAM_INT, FALSE, "new height" },
	P_END
};
static const GowlIpcParam p_opacity[] = {
	P_WIN_DEF,
	{ "opacity", GOWL_IPC_PARAM_NUMBER, TRUE, "0.0 transparent to 1.0" },
	P_END
};
static const GowlIpcParam p_set_tags[] = { P_WIN_DEF, P_TAGS, P_TAG, P_END };
static const GowlIpcParam p_send_to_monitor[] = {
	P_WIN_DEF,
	{ "monitor", GOWL_IPC_PARAM_MONITOR, TRUE, "the output to send it to" },
	P_TAGS, P_TAG, P_END
};
static const GowlIpcParam p_swap[] = {
	P_WIN_DEF,
	{ "with", GOWL_IPC_PARAM_WINDOW, TRUE, "the other window" },
	P_END
};
static const GowlIpcParam p_xy[] = {
	{ "x", GOWL_IPC_PARAM_NUMBER, TRUE, "layout x" },
	{ "y", GOWL_IPC_PARAM_NUMBER, TRUE, "layout y" },
	P_END
};
static const GowlIpcParam p_neighbour[] = {
	P_WIN_DEF,
	{ "direction", GOWL_IPC_PARAM_STRING, TRUE, "left, right, up or down" },
	P_END
};
static const GowlIpcParam p_children[] = {
	P_WIN_DEF,
	{ "recursive", GOWL_IPC_PARAM_BOOL, FALSE,
	  "dialogs of dialogs too" },
	P_END
};
static const GowlIpcParam p_type_text[] = {
	P_WIN_DEF,
	{ "text", GOWL_IPC_PARAM_STRING, TRUE, "UTF-8 text to type" },
	P_END
};
static const GowlIpcParam p_send_key[] = {
	P_WIN_DEF,
	{ "key", GOWL_IPC_PARAM_STRING, TRUE, "a chord: \"Ctrl+c\", \"Return\"" },
	P_END
};
static const GowlIpcParam p_win_shot[] = {
	P_WIN_DEF,
	{ "path", GOWL_IPC_PARAM_STRING, TRUE, "absolute path of the PNG" },
	P_END
};
static const GowlIpcParam p_mon_shot[] = {
	P_MON,
	{ "path", GOWL_IPC_PARAM_STRING, TRUE, "absolute path of the PNG" },
	P_END
};
static const GowlIpcParam p_layout_set[] = {
	P_MON,
	{ "name", GOWL_IPC_PARAM_STRING, TRUE, "a layout from layout.list" },
	P_END
};
static const GowlIpcParam p_mfact[] = {
	P_MON,
	{ "value", GOWL_IPC_PARAM_NUMBER, TRUE, "0.05 to 0.95" },
	{ "relative", GOWL_IPC_PARAM_BOOL, FALSE, "add value to the current one" },
	P_END
};
static const GowlIpcParam p_nmaster[] = {
	P_MON,
	{ "value", GOWL_IPC_PARAM_INT, TRUE, "windows in the master area" },
	{ "relative", GOWL_IPC_PARAM_BOOL, FALSE, "add value to the current one" },
	P_END
};
static const GowlIpcParam p_order[] = {
	{ "ids", GOWL_IPC_PARAM_ARRAY, TRUE, "window ids, first is master-most" },
	{ "tile", GOWL_IPC_PARAM_BOOL, FALSE,
	  "un-float and un-fullscreen them first" },
	P_END
};
static const GowlIpcParam p_apply[] = {
	{ "windows", GOWL_IPC_PARAM_ARRAY, TRUE,
	  "[{id, x, y, width, height, monitor?}] -- members as in "
	  "window.set_geometry" },
	P_MON,
	{ "relative_to", GOWL_IPC_PARAM_STRING, FALSE,
	  "\"area\" (default here), \"monitor\" or \"layout\"" },
	{ "units", GOWL_IPC_PARAM_STRING, FALSE, "\"px\" (default) or \"fraction\"" },
	P_END
};
static const GowlIpcParam p_mon_focus[] = {
	P_MON,
	{ "warp", GOWL_IPC_PARAM_BOOL, FALSE, "move the pointer there too" },
	P_END
};
static const GowlIpcParam p_mon_configure[] = {
	P_MON,
	{ "enabled", GOWL_IPC_PARAM_BOOL, FALSE, "turn the output on or off" },
	{ "scale", GOWL_IPC_PARAM_NUMBER, FALSE, "0.25 to 8" },
	{ "transform", GOWL_IPC_PARAM_INT, FALSE,
	  "0 normal, 1 90, 2 180, 3 270, 4-7 flipped" },
	{ "x", GOWL_IPC_PARAM_INT, FALSE, "layout x" },
	{ "y", GOWL_IPC_PARAM_INT, FALSE, "layout y" },
	{ "power", GOWL_IPC_PARAM_BOOL, FALSE, "DPMS on/off" },
	{ "hdr", GOWL_IPC_PARAM_BOOL, FALSE, "HDR on/off" },
	P_END
};
static const GowlIpcParam p_tags_set[] = { P_MON, P_TAGS, P_TAG, P_END };
static const GowlIpcParam p_warp[] = {
	{ "x", GOWL_IPC_PARAM_NUMBER, FALSE, "layout x (window-relative with window)" },
	{ "y", GOWL_IPC_PARAM_NUMBER, FALSE, "layout y" },
	{ "window", GOWL_IPC_PARAM_WINDOW, FALSE,
	  "warp into this window (its centre unless x/y)" },
	P_END
};
static const GowlIpcParam p_kbd_layout[] = {
	{ "layout", GOWL_IPC_PARAM_ANY, TRUE, "\"next\", \"prev\" or an index" },
	P_END
};
static const GowlIpcParam p_keymode[] = {
	{ "mode", GOWL_IPC_PARAM_STRING, TRUE, "a mode name; \"default\" leaves" },
	P_END
};
static const GowlIpcParam p_key[] = {
	{ "key", GOWL_IPC_PARAM_STRING, TRUE, "a bound chord: \"Super+Return\"" },
	P_END
};
static const GowlIpcParam p_action[] = {
	{ "name", GOWL_IPC_PARAM_STRING, TRUE, "an action nick: \"tag-view\"" },
	{ "arg", GOWL_IPC_PARAM_STRING, FALSE, "its argument, as in a keybind" },
	P_END
};
static const GowlIpcParam p_spawn[] = {
	{ "argv", GOWL_IPC_PARAM_ARRAY, FALSE, "the program and its arguments" },
	{ "command", GOWL_IPC_PARAM_STRING, FALSE,
	  "a command line, split like a shell would (instead of argv)" },
	{ "shell", GOWL_IPC_PARAM_BOOL, FALSE, "run command through /bin/sh -c" },
	{ "cwd", GOWL_IPC_PARAM_STRING, FALSE, "working directory" },
	{ "env", GOWL_IPC_PARAM_OBJECT, FALSE,
	  "variables to set (a null value unsets one)" },
	P_MON, P_TAGS, P_TAG,
	{ "floating", GOWL_IPC_PARAM_BOOL, FALSE,
	  "float its first window (implied by width/height)" },
	P_BOX,
	P_END
};
static const GowlIpcParam p_power[] = { P_TOGGLE, P_END };
static const GowlIpcParam p_transition_set[] = {
	{ "name", GOWL_IPC_PARAM_STRING, TRUE,
	  "a loaded transition, \"random\", \"none\", \"next\" or \"prev\"" },
	{ "duration", GOWL_IPC_PARAM_INT, FALSE,
	  "also set tag-transition-duration, in ms" },
	P_END
};

typedef struct {
	const gchar        *name;
	const gchar        *summary;
	const GowlIpcParam *params;
	const gchar        *result;
	GowlIpcMethodFunc   func;
} MethodDef;

static const MethodDef method_defs[] = {
	{ "gowl.version", "The compositor's and the protocol's versions",
	  p_none, "{version, protocol, protocol_version, wlroots, xwayland}",
	  m_gowl_version },
	{ "gowl.state", "Every output and window, the focus, the key mode",
	  p_none, "{monitors[], windows[], focused_window, ...}", m_gowl_state },
	{ "gowl.command", "Run any text command (module commands included)",
	  p_command, "{ok, reply, json?}", m_gowl_command },
	{ "gowl.reload", "Reload the configuration", p_none, "{problems}",
	  m_gowl_reload },

	{ "windows.list", "Every window, optionally filtered", p_windows_list,
	  "[window]", m_windows_list },
	{ "window.get", "One window", p_win_def, "window", m_window_get },
	{ "window.focused", "The focused window", p_none, "window|null",
	  m_window_focused },
	{ "window.focus", "Focus a window, switching to its tag and output",
	  p_win_req, "window", m_window_focus },
	{ "window.close", "Ask a window to close", p_win_req, "{ok}",
	  m_window_close },
	{ "window.set_geometry", "Float a window and place it", p_set_geometry,
	  "window", m_window_set_geometry },
	{ "window.move_by", "Float a window and nudge or grow it", p_move_by,
	  "window", m_window_move_by },
	{ "window.center", "Float a window centred on its output", p_center,
	  "window", m_window_center },
	{ "window.set_floating", "Float or tile a window", p_flag, "window",
	  m_window_set_floating },
	{ "window.set_fullscreen", "Fullscreen a window or not", p_flag,
	  "window", m_window_set_fullscreen },
	{ "window.set_sticky", "Pin a window to every tag (floats it)", p_flag,
	  "window", m_window_set_sticky },
	{ "window.set_below", "Keep a floating window behind the tiles", p_flag,
	  "window", m_window_set_below },
	{ "window.set_minimized", "Minimize or restore a window", p_minimize,
	  "window", m_window_set_minimized },
	{ "window.set_urgent", "Mark a window urgent or not", p_flag, "window",
	  m_window_set_urgent },
	{ "window.set_opacity", "A window's opacity", p_opacity, "window",
	  m_window_set_opacity },
	{ "window.set_tags", "Put a window on other tags", p_set_tags, "window",
	  m_window_set_tags },
	{ "window.send_to_monitor", "Move a window to another output",
	  p_send_to_monitor, "window", m_window_send_to_monitor },
	{ "window.swap", "Swap two windows' places in the tiling", p_swap,
	  "{ok}", m_window_swap },
	{ "window.promote", "Make a window the master", p_win_def, "window",
	  m_window_promote },
	{ "window.at", "The window at a layout point", p_xy, "window|null",
	  m_window_at },
	{ "window.neighbour", "The window beside one, in a direction",
	  p_neighbour, "window|null", m_window_neighbour },
	{ "window.children", "A window's dialogs (transient-for)", p_children,
	  "[window]", m_window_children },
	{ "window.process", "The process behind a window", p_win_def,
	  "{pid, ppid, comm, cmdline, cwd}", m_window_process },
	{ "window.type_text", "Type text into a window", p_type_text, "{sent}",
	  m_window_type_text },
	{ "window.send_key", "Send one key chord to a window", p_send_key,
	  "{ok}", m_window_send_key },
	{ "window.screenshot", "Save a window as a PNG", p_win_shot,
	  "{path, width, height}", m_window_screenshot },

	{ "layout.list", "The layouts there are", p_none, "[{name, symbol}]",
	  m_layout_list },
	{ "layout.get", "An output's layout", p_mon,
	  "{monitor, name, symbol, mfact, nmaster}", m_layout_get },
	{ "layout.set", "Select an output's layout", p_layout_set,
	  "{monitor, name, symbol, mfact, nmaster}", m_layout_set },
	{ "layout.set_mfact", "The master area's share", p_mfact,
	  "{monitor, name, symbol, mfact, nmaster}", m_layout_set_mfact },
	{ "layout.set_nmaster", "How many windows are masters", p_nmaster,
	  "{monitor, name, symbol, mfact, nmaster}", m_layout_set_nmaster },
	{ "layout.order", "Re-deal windows' tiling slots in this order",
	  p_order, "{ok}", m_layout_order },
	{ "layout.apply", "Place many windows at once, all or none",
	  p_apply, "[window]", m_layout_apply },

	{ "monitors.list", "Every output", p_none, "[monitor]",
	  m_monitors_list },
	{ "monitor.get", "One output", p_mon, "monitor", m_monitor_get },
	{ "monitor.focus", "Focus an output", p_mon_focus, "monitor",
	  m_monitor_focus },
	{ "monitor.configure", "An output's position, scale, rotation, power",
	  p_mon_configure, "monitor", m_monitor_configure },
	{ "monitor.screenshot", "Save an output as a PNG", p_mon_shot,
	  "{path, width, height}", m_monitor_screenshot },

	{ "tags.get", "An output's tags", p_mon,
	  "{monitor, active, previous, occupied, urgent}", m_tags_get },
	{ "tags.view", "View tags on an output", p_tags_set,
	  "{monitor, active, previous, occupied, urgent}", m_tags_view },
	{ "tags.toggle", "Add or remove tags from the view", p_tags_set,
	  "{monitor, active, previous, occupied, urgent}", m_tags_toggle },

	{ "pointer.get", "Where the pointer is and what is under it", p_none,
	  "{x, y, window, confined}", m_pointer_get },
	{ "pointer.warp", "Move the pointer", p_warp,
	  "{x, y, window, confined}", m_pointer_warp },
	{ "keyboard.get_layout", "The keyboard layout", p_none, "{name, index}",
	  m_keyboard_get_layout },
	{ "keyboard.set_layout", "Switch keyboard layout", p_kbd_layout,
	  "{name, index}", m_keyboard_set_layout },
	{ "keymode.get", "The key mode in force", p_none, "string",
	  m_keymode_get },
	{ "keymode.set", "Enter a key mode", p_keymode, "string",
	  m_keymode_set },
	{ "keybinds.list", "Every key binding", p_none,
	  "[{key, action, arg, desc, mode}]", m_keybinds_list },
	{ "key.dispatch", "Run what a chord is bound to", p_key, "bool",
	  m_key_dispatch },
	{ "action.run", "Run a keybind action by name", p_action, "bool",
	  m_action_run },

	{ "spawn", "Start a program and say where its window goes", p_spawn,
	  "{pid}", m_spawn },

	{ "transition.get", "The tag transition in force and the loaded ones",
	  p_none, "{current, label, duration, transitions[{name, label}]}",
	  m_transition_get },
	{ "transition.set", "Select the tag transition (as Super+: does)",
	  p_transition_set, "as transition.get", m_transition_set },

	{ "session.lock", "Lock the session", p_none, "{ok}", m_session_lock },
	{ "session.locked", "Whether the session is locked", p_none, "bool",
	  m_session_locked },
	{ "outputs.power", "Power every output on or off", p_power, "bool",
	  m_outputs_power },

	{ NULL, NULL, NULL, NULL, NULL }
};

/**
 * gowl_compositor_ipc_install_methods:
 * @self: the compositor
 * @ipc: the server to install into
 *
 * Registers the wire protocol's methods and event formatter on @ipc and
 * connects the signals that feed `window.changed', `tags.changed',
 * `layout.changed' and `monitor.changed'.  Called by
 * gowl_compositor_set_ipc(), so both launch paths -- gowl's own main()
 * and cmacs's `gowl-start-ipc' -- get the same protocol.
 */
void
gowl_compositor_ipc_install_methods(GowlCompositor *self, GowlIpc *ipc)
{
	GowlIpcMethodsState *st;
	guint i;

	g_return_if_fail(GOWL_IS_COMPOSITOR(self));
	g_return_if_fail(GOWL_IS_IPC(ipc));

	st = g_new0(GowlIpcMethodsState, 1);
	g_weak_ref_init(&st->compositor, self);
	st->windows = g_hash_table_new_full(g_direct_hash, g_direct_equal,
	                                    NULL, win_snap_free);
	st->monitors = g_hash_table_new_full(g_str_hash, g_str_equal,
	                                     g_free, mon_snap_free);
	/* Owned by the server: replacing it (a second install) frees the
	 * old one, and it goes when the server does. */
	g_object_set_data_full(G_OBJECT(ipc), GOWL_IPC_METHODS_KEY, st,
	                       methods_state_free);

	for (i = 0; method_defs[i].name != NULL; i++)
		gowl_ipc_register_method(ipc, method_defs[i].name,
		                         method_defs[i].summary,
		                         method_defs[i].params,
		                         method_defs[i].result,
		                         method_defs[i].func, st, NULL);

	gowl_ipc_set_event_formatter(ipc, format_event, st);
	declare_events(ipc);

	/* The starting picture, so the first change after this is
	 * announced as a change.  Nothing is pushed: nobody can have
	 * subscribed to a server that had no methods a moment ago. */
	{
		GList *l;

		for (l = self->monitors; l != NULL; l = l->next)
			diff_monitor(ipc, st, self, (GowlMonitor *)l->data);
		diff_windows(ipc, st, self);
	}

	/* Disconnected automatically when the server is finalized, and
	 * explicitly by gowl_compositor_ipc_uninstall_methods() when the
	 * compositor is handed another one. */
	g_signal_connect_object(self, "arranged", G_CALLBACK(on_arranged),
	                        ipc, 0);
	g_signal_connect_object(self, "frame-rendered",
	                        G_CALLBACK(on_frame_rendered), ipc, 0);
}

/**
 * gowl_compositor_ipc_uninstall_methods:
 * @self: the compositor
 * @ipc: a server gowl_compositor_ipc_install_methods() was called on
 *
 * Stops feeding @ipc events from @self.  The methods stay registered
 * but answer that the compositor is gone once it is.
 */
void
gowl_compositor_ipc_uninstall_methods(GowlCompositor *self, GowlIpc *ipc)
{
	g_return_if_fail(GOWL_IS_COMPOSITOR(self));
	g_return_if_fail(GOWL_IS_IPC(ipc));

	g_signal_handlers_disconnect_by_data(self, ipc);
}
