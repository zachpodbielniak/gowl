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
 * The `gabbrev' widget: text expansion at a glance, and the place its
 * toasts lead to.  See docs/bar-widgets/gabbrev.org.
 *
 * The bar shows a keyboard: muted while expansion is off, a count while
 * expansions are still resolving (a slow command, a model, a macro).
 * The panel lists those expansions with a cancel for each, the phrases
 * learning has noticed with accept and dismiss, the most used matches,
 * and how many have never fired.  Middle-click toggles expansion.
 *
 * Everything comes from the gabbrev module's own commands, run in
 * process (gabbrev-status, gabbrev-suggestions): this file owns no
 * state the module does not, so the widget and `gowl-msg' can never
 * disagree.  The poll runs on the compositor thread, as every module
 * command does, and is one status call every two seconds.
 *
 * Reading a reply is split out (bar_gabbrev_apply_status(),
 * bar_gabbrev_apply_suggestions()) and exported, so a test drives the
 * widget with JSON and no compositor.
 */

#include <linux/input-event-codes.h>
#include <xkbcommon/xkbcommon-keysyms.h>
#include <json-glib/json-glib.h>
#include <string.h>

#include "core/gowl-compositor.h"
#include "barkit/gowl-bar-panel.h"
#include "barkit/gowl-bar-toast.h"

#include "bar-internal.h"

/* nf-fa-keyboard */
#define GABW_ICON          "\xef\x84\x9c"
/* nf-fa-hourglass_half */
#define GABW_ICON_PENDING  "\xef\x89\x92"
/* nf-fa-lightbulb */
#define GABW_ICON_IDEA     "\xef\x83\xab"
#define GABW_MAX_ROWS      (8)

/* One expansion still resolving */
typedef struct {
	gint64   id;
	gchar   *match;
	gchar   *waiting;     /* the variable type it waits for, or NULL */
	gdouble  seconds;
} GabwJob;

/* One phrase learning noticed */
typedef struct {
	gchar   *phrase;
	gchar   *trigger;     /* NULL when no free trigger was found */
	gint64   count;
} GabwSuggestion;

/* One of the most used matches */
typedef struct {
	gchar   *id;
	gint64   uses;
} GabwTop;

typedef struct {
	gboolean   available;   /* the module answered */
	gboolean   on;
	gint64     n_matches;
	gint64     n_suggestions;
	gint64     never_used;
	gboolean   learning;
	GPtrArray *jobs;        /* GabwJob* */
	GPtrArray *suggestions; /* GabwSuggestion* */
	GPtrArray *top;         /* GabwTop* */
} GabwData;

static void
gabw_job_free(gpointer p)
{
	GabwJob *j = p;

	g_free(j->match);
	g_free(j->waiting);
	g_free(j);
}

static void
gabw_suggestion_free(gpointer p)
{
	GabwSuggestion *s = p;

	g_free(s->phrase);
	g_free(s->trigger);
	g_free(s);
}

static void
gabw_top_free(gpointer p)
{
	GabwTop *t = p;

	g_free(t->id);
	g_free(t);
}

static gpointer
gabw_create(GowlBarPlugin *plugin)
{
	GabwData *d;

	(void)plugin;
	d = g_new0(GabwData, 1);
	d->jobs = g_ptr_array_new_with_free_func(gabw_job_free);
	d->suggestions = g_ptr_array_new_with_free_func(gabw_suggestion_free);
	d->top = g_ptr_array_new_with_free_func(gabw_top_free);
	return d;
}

static void
gabw_destroy(GowlBarPlugin *plugin, gpointer data)
{
	GabwData *d = data;

	(void)plugin;
	if (d == NULL)
		return;
	g_ptr_array_unref(d->jobs);
	g_ptr_array_unref(d->suggestions);
	g_ptr_array_unref(d->top);
	g_free(d);
}

/* ── Reading the module's replies ─────────────────────────────────── */

/* A member of @o, or a fallback when it is missing or not that type */
static const gchar *
gabw_str(
	JsonObject  *o,
	const gchar *key
){
	JsonNode *n = o != NULL ? json_object_get_member(o, key) : NULL;

	if (n == NULL || !JSON_NODE_HOLDS_VALUE(n)
	    || json_node_get_value_type(n) != G_TYPE_STRING)
		return NULL;
	return json_node_get_string(n);
}

static gint64
gabw_int(
	JsonObject  *o,
	const gchar *key
){
	JsonNode *n = o != NULL ? json_object_get_member(o, key) : NULL;

	if (n == NULL || !JSON_NODE_HOLDS_VALUE(n))
		return 0;
	if (json_node_get_value_type(n) == G_TYPE_INT64)
		return json_node_get_int(n);
	if (json_node_get_value_type(n) == G_TYPE_DOUBLE)
		return (gint64)json_node_get_double(n);
	return 0;
}

static gboolean
gabw_bool(
	JsonObject  *o,
	const gchar *key
){
	JsonNode *n = o != NULL ? json_object_get_member(o, key) : NULL;

	return n != NULL && JSON_NODE_HOLDS_VALUE(n)
	       && json_node_get_value_type(n) == G_TYPE_BOOLEAN
	       && json_node_get_boolean(n);
}

static gdouble
gabw_double(
	JsonObject  *o,
	const gchar *key
){
	JsonNode *n = o != NULL ? json_object_get_member(o, key) : NULL;

	if (n == NULL || !JSON_NODE_HOLDS_VALUE(n))
		return 0.0;
	if (json_node_get_value_type(n) == G_TYPE_DOUBLE)
		return json_node_get_double(n);
	if (json_node_get_value_type(n) == G_TYPE_INT64)
		return (gdouble)json_node_get_int(n);
	return 0.0;
}

/* json-glib's typed getters assert on a member of another type: a
   reply is only ever read through these, which say NULL instead */
static JsonObject *
gabw_object(
	JsonObject  *o,
	const gchar *key
){
	JsonNode *n = o != NULL ? json_object_get_member(o, key) : NULL;

	return n != NULL && JSON_NODE_HOLDS_OBJECT(n) ? json_node_get_object(n)
	                                              : NULL;
}

static JsonObject *
gabw_element(
	JsonArray *arr,
	guint      i
){
	JsonNode *n = json_array_get_element(arr, i);

	return n != NULL && JSON_NODE_HOLDS_OBJECT(n) ? json_node_get_object(n)
	                                              : NULL;
}

static JsonArray *
gabw_array(
	JsonObject  *o,
	const gchar *key
){
	JsonNode *n = o != NULL ? json_object_get_member(o, key) : NULL;

	return n != NULL && JSON_NODE_HOLDS_ARRAY(n) ? json_node_get_array(n)
	                                             : NULL;
}

/* The JSON after `OK ', parsed; NULL for anything else */
static JsonNode *
gabw_parse(const gchar *reply)
{
	g_autoptr(JsonParser) p = NULL;

	if (reply == NULL)
		return NULL;
	if (g_str_has_prefix(reply, "OK "))
		reply += 3;
	p = json_parser_new();
	if (!json_parser_load_from_data(p, reply, -1, NULL)
	    || json_parser_get_root(p) == NULL)
		return NULL;
	return json_node_copy(json_parser_get_root(p));
}

/* What the bar shows, from the data */
static void
gabw_refresh_label(
	GowlBarPlugin *plugin,
	GabwData      *d
){
	g_autofree gchar *tip = NULL;
	gchar buf[32];

	gowl_bar_plugin_set_icon(plugin, d->jobs->len > 0 ? GABW_ICON_PENDING
	                                                  : GABW_ICON);
	if (!d->available) {
		gowl_bar_plugin_set_label(plugin, NULL);
		gowl_bar_plugin_set_color(plugin, GOWL_BAR_COLOR_MUTED);
		gowl_bar_plugin_set_tooltip(plugin,
			"Text expansion: the gabbrev module is not loaded");
		return;
	}
	if (d->jobs->len > 0) {
		g_snprintf(buf, sizeof buf, "%u", d->jobs->len);
		gowl_bar_plugin_set_label(plugin, buf);
		gowl_bar_plugin_set_color(plugin, GOWL_BAR_COLOR_YELLOW);
	} else {
		gowl_bar_plugin_set_label(plugin, NULL);
		gowl_bar_plugin_set_color(plugin, d->on ? GOWL_BAR_COLOR_TEXT
		                                       : GOWL_BAR_COLOR_MUTED);
	}
	tip = g_strdup_printf("Text expansion %s -- %" G_GINT64_FORMAT
	                      " match(es)%s%s%s",
	                      d->on ? "on" : "off", d->n_matches,
	                      d->jobs->len > 0 ? ", expanding now" : "",
	                      d->n_suggestions > 0 ? ", phrases to accept" : "",
	                      "; middle-click toggles");
	gowl_bar_plugin_set_tooltip(plugin, tip);
}

/**
 * bar_gabbrev_apply_status:
 * @plugin: the widget
 * @data: its instance data
 * @reply: (nullable): what `gabbrev-status' answered, or %NULL when the
 *   module is not there
 *
 * Takes in a status reply and updates the bar.  Exported for the test.
 */
void
bar_gabbrev_apply_status(
	GowlBarPlugin *plugin,
	gpointer       data,
	const gchar   *reply
){
	GabwData *d = data;
	g_autoptr(JsonNode) root = gabw_parse(reply);
	JsonObject *o, *stats, *learning;
	JsonArray *arr;
	guint i;

	if (d == NULL)
		return;
	g_ptr_array_set_size(d->jobs, 0);
	g_ptr_array_set_size(d->top, 0);
	d->available = root != NULL && JSON_NODE_HOLDS_OBJECT(root);
	if (!d->available) {
		gabw_refresh_label(plugin, d);
		return;
	}
	o = json_node_get_object(root);
	d->on = gabw_bool(o, "on");
	d->n_matches = gabw_int(o, "matches");

	arr = gabw_array(o, "jobs");
	for (i = 0; arr != NULL && i < json_array_get_length(arr); i++) {
		JsonObject *jo = gabw_element(arr, i);
		GabwJob *j;

		if (jo == NULL)
			continue;
		j = g_new0(GabwJob, 1);
		j->id = gabw_int(jo, "id");
		j->match = g_strdup(gabw_str(jo, "match"));
		j->waiting = g_strdup(gabw_str(jo, "waiting-for"));
		j->seconds = gabw_double(jo, "seconds");
		g_ptr_array_add(d->jobs, j);
	}

	stats = gabw_object(o, "stats");
	d->never_used = gabw_int(stats, "never-used");
	arr = gabw_array(stats, "top");
	for (i = 0; arr != NULL && i < json_array_get_length(arr); i++) {
		JsonObject *to = gabw_element(arr, i);
		GabwTop *t;

		if (to == NULL || gabw_str(to, "id") == NULL)
			continue;
		t = g_new0(GabwTop, 1);
		t->id = g_strdup(gabw_str(to, "id"));
		t->uses = gabw_int(to, "uses");
		g_ptr_array_add(d->top, t);
	}

	learning = gabw_object(o, "learning");
	d->learning = gabw_bool(learning, "on");
	d->n_suggestions = gabw_int(learning, "suggestions");
	gabw_refresh_label(plugin, d);
}

/**
 * bar_gabbrev_apply_suggestions:
 * @plugin: the widget
 * @data: its instance data
 * @reply: (nullable): what `gabbrev-suggestions' answered
 *
 * Takes in the suggestions the panel lists.  Exported for the test.
 */
void
bar_gabbrev_apply_suggestions(
	GowlBarPlugin *plugin,
	gpointer       data,
	const gchar   *reply
){
	GabwData *d = data;
	g_autoptr(JsonNode) root = gabw_parse(reply);
	JsonArray *arr;
	guint i;

	(void)plugin;
	if (d == NULL)
		return;
	g_ptr_array_set_size(d->suggestions, 0);
	if (root == NULL || !JSON_NODE_HOLDS_ARRAY(root))
		return;
	arr = json_node_get_array(root);
	for (i = 0; i < json_array_get_length(arr); i++) {
		JsonObject *so = gabw_element(arr, i);
		GabwSuggestion *s;

		if (so == NULL || gabw_str(so, "phrase") == NULL)
			continue;
		s = g_new0(GabwSuggestion, 1);
		s->phrase = g_strdup(gabw_str(so, "phrase"));
		s->trigger = g_strdup(gabw_str(so, "trigger"));
		s->count = gabw_int(so, "count");
		g_ptr_array_add(d->suggestions, s);
	}
}

/* ── Talking to the module ────────────────────────────────────────── */

/* A gabbrev command, in process.  NULL when there is no compositor (a
   test), which callers treat as "nothing new". */
static gchar *
gabw_command(const gchar *line)
{
	const BarEnv *env = bar_env();

	if (env == NULL || env->compositor == NULL)
		return NULL;
	return gowl_compositor_run_command(GOWL_COMPOSITOR(env->compositor),
	                                   line);
}

static gint
gabw_interval(GowlBarPlugin *plugin, gpointer data)
{
	(void)plugin;
	(void)data;
	return 2;
}

static void
gabw_poll(GowlBarPlugin *plugin, gpointer data)
{
	g_autofree gchar *reply = gabw_command("gabbrev-status");

	if (reply == NULL)
		return;
	/* `ERROR unknown command' when the module is not loaded */
	bar_gabbrev_apply_status(plugin, data,
	                         g_str_has_prefix(reply, "OK ") ? reply : NULL);
}

/* Middle-click: expansion on or off, without opening anything */
static gboolean
gabw_click(GowlBarPlugin *plugin, gpointer data, guint button, gint x,
           gint y, guint modifiers)
{
	g_autofree gchar *reply = NULL;

	(void)x;
	(void)y;
	(void)modifiers;
	if (button != BTN_MIDDLE)
		return FALSE;
	reply = gabw_command("gabbrev-toggle");
	gabw_poll(plugin, data);
	return TRUE;
}

/* ── The panel ────────────────────────────────────────────────────── */

static GowlBarPanel *
gabw_panel(GowlBarPlugin *plugin, gpointer data)
{
	GabwData *d = data;
	GowlBarPanel *panel;
	GowlBarPanelItem *item;
	g_autofree gchar *sub = NULL;
	guint i;

	/* The newest list each time the panel is built */
	{
		g_autofree gchar *reply = gabw_command("gabbrev-suggestions 8");

		if (reply != NULL)
			bar_gabbrev_apply_suggestions(plugin, d, reply);
	}

	panel = gowl_bar_panel_new();
	gowl_bar_panel_set_width(panel, 420);
	if (d == NULL || !d->available) {
		gowl_bar_panel_add_hero(panel, GABW_ICON, "Text expansion",
		                        "the gabbrev module is not loaded");
		return panel;
	}
	sub = g_strdup_printf("%s -- %" G_GINT64_FORMAT " match(es)",
	                      d->on ? "On" : "Off", d->n_matches);
	gowl_bar_panel_add_hero(panel, GABW_ICON, "Text expansion", sub);
	item = gowl_bar_panel_add_buttons(panel, "act");
	gowl_bar_panel_add_button(item, d->on ? "Turn off" : "Turn on", FALSE);
	gowl_bar_panel_add_button(item, "Reload", FALSE);

	/* Expansions still resolving: Return (or a click) cancels one */
	if (d->jobs->len > 0) {
		gowl_bar_panel_add_section(panel, "Expanding now");
		for (i = 0; i < d->jobs->len && i < GABW_MAX_ROWS; i++) {
			GabwJob *j = g_ptr_array_index(d->jobs, i);
			g_autofree gchar *id = g_strdup_printf("job:%" G_GINT64_FORMAT,
			                                       j->id);
			g_autofree gchar *what = g_strdup_printf(
				"waiting for %s -- %.1f s -- select to cancel",
				j->waiting != NULL ? j->waiting : "the expansion",
				j->seconds);

			gowl_bar_panel_add_row(panel, id, GABW_ICON_PENDING, j->match,
			                       what);
		}
		item = gowl_bar_panel_add_buttons(panel, "cancel-all");
		gowl_bar_panel_add_button(item, "Cancel all", FALSE);
	}

	/* Phrases learning noticed: select accepts, x dismisses */
	if (d->suggestions->len > 0) {
		gowl_bar_panel_add_section(panel, "Typed often -- a for accept, "
		                           "x to dismiss");
		for (i = 0; i < d->suggestions->len && i < GABW_MAX_ROWS; i++) {
			GabwSuggestion *s = g_ptr_array_index(d->suggestions, i);
			g_autofree gchar *id = g_strdup_printf("sug:%u", i);
			g_autofree gchar *what = g_strdup_printf(
				"%s -- typed %" G_GINT64_FORMAT " times",
				s->trigger != NULL ? s->trigger : "no free trigger",
				s->count);

			gowl_bar_panel_add_row(panel, id, GABW_ICON_IDEA, s->phrase, what);
		}
	} else if (d->learning) {
		gowl_bar_panel_add_field(panel, "Learning", "nothing repeated yet");
	}

	/* What is used, and what never is */
	if (d->top->len > 0 || d->never_used > 0) {
		gowl_bar_panel_add_section(panel, "Usage");
		for (i = 0; i < d->top->len && i < 3; i++) {
			GabwTop *t = g_ptr_array_index(d->top, i);
			g_autofree gchar *n = g_strdup_printf("%" G_GINT64_FORMAT " uses",
			                                      t->uses);

			gowl_bar_panel_add_field(panel, t->id, n);
		}
		if (d->never_used > 0) {
			g_autofree gchar *n = g_strdup_printf("%" G_GINT64_FORMAT
			                                      " match(es)", d->never_used);

			gowl_bar_panel_add_field(panel, "Never used", n);
		}
	}
	return panel;
}

/* The suggestion a `sug:N' id names, or NULL */
static GabwSuggestion *
gabw_suggestion_for(
	GabwData    *d,
	const gchar *item_id
){
	guint64 i;

	if (d == NULL || item_id == NULL || !g_str_has_prefix(item_id, "sug:")
	    || !g_ascii_string_to_unsigned(item_id + 4, 10, 0, G_MAXUINT, &i, NULL)
	    || i >= d->suggestions->len)
		return NULL;
	return g_ptr_array_index(d->suggestions, i);
}

/* Accept a suggestion: the module writes it to the learned file */
static void
gabw_accept(
	GowlBarPlugin  *plugin,
	GabwSuggestion *s
){
	g_autofree gchar *line = s->trigger != NULL
		? g_strdup_printf("gabbrev-accept --trigger %s %s", s->trigger,
		                  s->phrase)
		: g_strdup_printf("gabbrev-accept %s", s->phrase);
	g_autofree gchar *reply = gabw_command(line);

	if (reply != NULL && g_str_has_prefix(reply, "OK "))
		gowl_bar_plugin_notify(plugin, GOWL_BAR_TOAST_LOW,
		                       "New text expansion", reply + 3);
	else
		gowl_bar_plugin_notify(plugin, GOWL_BAR_TOAST_NORMAL,
		                       "Not accepted", reply != NULL ? reply
		                       : "gabbrev is not running");
}

static void
gabw_action(GowlBarPlugin *plugin, gpointer data, const gchar *item_id,
            gint index, gdouble value, guint button)
{
	GabwData *d = data;
	GabwSuggestion *s;
	g_autofree gchar *reply = NULL;

	(void)value;
	(void)button;
	if (item_id == NULL)
		return;
	if (g_strcmp0(item_id, "act") == 0) {
		reply = gabw_command(index == 0 ? "gabbrev-toggle" : "gabbrev-reload");
	} else if (g_strcmp0(item_id, "cancel-all") == 0) {
		reply = gabw_command("gabbrev-cancel");
	} else if (g_str_has_prefix(item_id, "job:")) {
		g_autofree gchar *line = g_strdup_printf("gabbrev-cancel %s",
		                                         item_id + 4);

		reply = gabw_command(line);
	} else if ((s = gabw_suggestion_for(d, item_id)) != NULL) {
		gabw_accept(plugin, s);
	}
	gabw_poll(plugin, data);
	gowl_bar_plugin_request_panel_refresh(plugin);
}

/* `a' accepts and `x' dismisses the focused suggestion */
static gboolean
gabw_panel_key(GowlBarPlugin *plugin, gpointer data, guint keysym,
               guint modifiers, gint focused_item)
{
	GabwData *d = data;
	g_autoptr(GowlBarPanel) panel = NULL;
	GowlBarPanelItem *item;
	GabwSuggestion *s;
	const gchar *id;

	(void)modifiers;
	if (d == NULL || focused_item < 0)
		return FALSE;
	/* The focused item's id: the panel as last built */
	panel = gabw_panel(plugin, data);
	item = gowl_bar_panel_get_item(panel, (guint)focused_item);
	id = item != NULL ? gowl_bar_panel_item_get_id(item) : NULL;
	s = gabw_suggestion_for(d, id);
	if (s == NULL)
		return FALSE;
	if (keysym == XKB_KEY_a) {
		gabw_accept(plugin, s);
	} else if (keysym == XKB_KEY_x || keysym == XKB_KEY_Delete) {
		g_autofree gchar *line = g_strdup_printf("gabbrev-dismiss %s",
		                                         s->phrase);
		g_autofree gchar *reply = gabw_command(line);

		/* Gone from the list now, not at the next build */
		g_ptr_array_remove(d->suggestions, s);
	} else {
		return FALSE;
	}
	gowl_bar_plugin_request_panel_refresh(plugin);
	return TRUE;
}

static const GowlBarPluginVTable gabbrev_vtable = {
	sizeof(GowlBarPluginVTable),
	gabw_create, gabw_destroy,
	NULL, NULL, NULL,
	gabw_interval, gabw_poll, NULL,
	NULL, NULL,
	gabw_click, NULL,
	gabw_panel, gabw_action,
	NULL, NULL,
	gabw_panel_key,
	NULL,
	NULL, NULL
};

/**
 * bar_register_gabbrev_plugins:
 * @registry: the registry to populate
 */
void
bar_register_gabbrev_plugins(GowlBarRegistry *registry)
{
	gowl_bar_registry_register_vtable(registry, "gabbrev", "Text expansion",
		"gabbrev at a glance: on/off, expansions still resolving (with "
		"cancel), phrases to accept, usage", &gabbrev_vtable);
}
