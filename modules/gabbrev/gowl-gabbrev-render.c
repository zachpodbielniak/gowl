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
 * gowl-gabbrev-render.c - Variables, in order, then the replacement.
 *
 * The PLAN is the list of variables to resolve, built once:
 *
 *   1. global variables the replacement (or a local variable's params)
 *      names with {{...}}, in their own order, unless a local variable
 *      of the same name shadows them -- espanso resolves globals first;
 *   2. the match's own variables, in order; `type: global' puts the
 *      named global variable here instead of up front;
 *   3. and before any variable, whatever it names in `depends_on'.
 *
 * Then one step at a time.  A variable's params have the values so far
 * substituted into every string (unless `inject_vars: false'), so
 * `cmd: "echo {{name}}"' sees the variable before it.
 *
 * Ownership around an answer that comes from another thread: the render
 * holds the request while it waits; the request holds the render (as
 * its done data, released when the REQUEST is freed).  Delivering or
 * cancelling drops the render's side, so neither can be freed under the
 * other, and nothing is left once the provider lets go.
 */

#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "gowl-gabbrev"

#include "gowl-gabbrev-render.h"

#include <json-glib/json-glib.h>
#include <string.h>

struct _GabbrevRender {
	gint                      ref_count;
	GabbrevSet               *set;
	GabbrevMatch             *match;
	GHashTable               *captures;
	gchar                    *typed;
	GabbrevTypedCase          typed_case;
	gchar                    *app_id;
	gchar                    *title;
	guint                     depth;

	const GabbrevRenderHost  *host;
	gpointer                  host_data;
	GabbrevRenderDone         done;
	gpointer                  done_data;

	GPtrArray                *plan;      /* const GabbrevVar*, borrowed */
	guint                     step;
	GHashTable               *vars;      /* name (and name.field) -> value */
	GowlGabbrevRequest       *pending;   /* a reference */
	GabbrevRender            *child;     /* a nested `match' render */
	gboolean                  started;
	gboolean                  in_advance;
	gboolean                  finished;
	gboolean                  cancelled;

	gchar                    *text;
	gchar                    *html;
	gchar                    *error;
	gint                      cursor_back;
};

static void advance (GabbrevRender *r);

/* ── Pure helpers ────────────────────────────────────────────────────── */

static GRegex *
placeholder_regex(void)
{
	static GRegex *re;

	if (g_once_init_enter(&re)) {
		GRegex *r = g_regex_new(
			"\\{\\{\\s*([A-Za-z0-9_\\-]+(?:\\.[A-Za-z0-9_\\-]+)*)\\s*\\}\\}",
			G_REGEX_OPTIMIZE, 0, NULL);

		g_once_init_leave(&re, r);
	}
	return re;
}

typedef struct {
	GHashTable *vars;
	GPtrArray  *missing;
} SubstData;

static gboolean
subst_cb(
	const GMatchInfo *mi,
	GString          *out,
	gpointer          data
){
	SubstData *d = data;
	g_autofree gchar *name = g_match_info_fetch(mi, 1);
	const gchar *v = d->vars != NULL ? g_hash_table_lookup(d->vars, name) : NULL;

	if (v != NULL) {
		g_string_append(out, v);
	} else {
		g_autofree gchar *whole = g_match_info_fetch(mi, 0);

		g_string_append(out, whole);
		if (d->missing != NULL)
			g_ptr_array_add(d->missing, g_strdup(name));
	}
	return FALSE;
}

/**
 * gabbrev_substitute: (skip)
 * @text: text with {{name}} and {{name.field}} placeholders
 * @vars: (nullable): values by name
 * @missing: (nullable) (element-type utf8): collects names with no value
 *
 * A placeholder with no value is left as written.
 *
 * Returns: (transfer full): @text with values in
 */
gchar *
gabbrev_substitute(
	const gchar *text,
	GHashTable  *vars,
	GPtrArray   *missing
){
	SubstData d;
	gchar *out;

	if (text == NULL)
		return NULL;
	d.vars = vars;
	d.missing = missing;
	out = g_regex_replace_eval(placeholder_regex(), text, -1, 0, 0,
	                           subst_cb, &d, NULL);
	return out != NULL ? out : g_strdup(text);
}

/**
 * gabbrev_referenced_names: (skip)
 * @text: (nullable): text with placeholders
 *
 * Returns: (transfer full) (element-type utf8): the variable names it
 *   uses, each once, the part before any `.'
 */
GPtrArray *
gabbrev_referenced_names(const gchar *text)
{
	GPtrArray *out = g_ptr_array_new_with_free_func(g_free);
	g_autoptr(GMatchInfo) mi = NULL;

	if (text == NULL || !g_regex_match(placeholder_regex(), text, 0, &mi))
		return out;
	while (g_match_info_matches(mi)) {
		g_autofree gchar *name = g_match_info_fetch(mi, 1);
		gchar *dot = strchr(name, '.');

		if (dot != NULL)
			*dot = '\0';
		if (!g_ptr_array_find_with_equal_func(out, name, g_str_equal, NULL))
			g_ptr_array_add(out, g_strdup(name));
		g_match_info_next(mi, NULL);
	}
	return out;
}

static const gchar *
ordinal_suffix(gint day)
{
	if (day % 100 >= 11 && day % 100 <= 13)
		return "th";
	switch (day % 10) {
	case 1:  return "st";
	case 2:  return "nd";
	case 3:  return "rd";
	default: return "th";
	}
}

/**
 * gabbrev_format_date: (skip)
 * @format: a strftime-style format as g_date_time_format() takes it,
 *   plus `%o' (the day's ordinal suffix: st, nd, rd, th) and chrono's
 *   `%.3f', `%.6f', `%.9f' (fractional seconds), which espanso has
 * @when: the time
 *
 * Returns: (transfer full) (nullable): the text, %NULL for a format
 *   GLib refuses
 */
gchar *
gabbrev_format_date(
	const gchar *format,
	GDateTime   *when
){
	g_autoptr(GString) fmt = g_string_new(NULL);
	const gchar *p;

	for (p = format; *p != '\0'; p++) {
		if (*p != '%' || p[1] == '\0') {
			g_string_append_c(fmt, *p);
			continue;
		}
		if (p[1] == 'o') {
			g_string_append(fmt, ordinal_suffix(
				g_date_time_get_day_of_month(when)));
			p++;
		} else if (p[1] == '%') {
			g_string_append(fmt, "%%");
			p++;
		} else if (p[1] == '.' && g_ascii_isdigit(p[2]) && p[3] == 'f') {
			gint digits = p[2] - '0';
			gint usec = g_date_time_get_microsecond(when);
			gchar buf[16];

			if (digits <= 6)
				g_snprintf(buf, sizeof buf, "%06d", usec);
			else
				g_snprintf(buf, sizeof buf, "%06d000", usec);
			g_string_append_len(fmt, buf, MIN(digits, 9));
			p += 3;
		} else {
			g_string_append_c(fmt, '%');
		}
	}
	return g_date_time_format(when, fmt->str);
}

/**
 * gabbrev_take_cursor_hint: (skip)
 * @text: a rendered replacement
 * @out_back: (out): characters from the end to the hint, -1 for none
 *
 * Returns: (transfer full): @text without the hint (every copy of it;
 *   only the first one counts, as in espanso)
 */
gchar *
gabbrev_take_cursor_hint(
	const gchar *text,
	gint        *out_back
){
	const gchar *hint;
	g_auto(GStrv) parts = NULL;
	g_autofree gchar *after = NULL;

	*out_back = -1;
	hint = strstr(text, GABBREV_CURSOR_HINT);
	if (hint == NULL)
		return g_strdup(text);
	parts = g_strsplit(hint + strlen(GABBREV_CURSOR_HINT),
	                   GABBREV_CURSOR_HINT, -1);
	after = g_strjoinv("", parts);
	*out_back = (gint)g_utf8_strlen(after, -1);
	{
		g_autofree gchar *before = g_strndup(text, (gsize)(hint - text));

		return g_strconcat(before, after, NULL);
	}
}

/* ── Params ──────────────────────────────────────────────────────────── */

/* Every string inside @node with the values so far substituted. */
static void
json_substitute(
	JsonNode   *node,
	GHashTable *vars
){
	if (node == NULL)
		return;
	switch (json_node_get_node_type(node)) {
	case JSON_NODE_VALUE:
		if (json_node_get_value_type(node) == G_TYPE_STRING) {
			g_autofree gchar *s = gabbrev_substitute(
				json_node_get_string(node), vars, NULL);

			json_node_set_string(node, s);
		}
		break;
	case JSON_NODE_ARRAY: {
		JsonArray *a = json_node_get_array(node);
		guint i;

		for (i = 0; i < json_array_get_length(a); i++)
			json_substitute(json_array_get_element(a, i), vars);
		break;
	}
	case JSON_NODE_OBJECT: {
		JsonObject *o = json_node_get_object(node);
		g_autoptr(GList) members = json_object_get_members(o);
		GList *l;

		for (l = members; l != NULL; l = l->next)
			json_substitute(json_object_get_member(o, l->data), vars);
		break;
	}
	case JSON_NODE_NULL:
	default:
		break;
	}
}

/* One param as text (see gowl_gabbrev_request_get_param()). */
static gchar *
json_as_text(JsonNode *node)
{
	if (node == NULL || JSON_NODE_HOLDS_NULL(node))
		return g_strdup("");
	if (JSON_NODE_HOLDS_VALUE(node)) {
		GType t = json_node_get_value_type(node);

		if (t == G_TYPE_STRING)
			return g_strdup(json_node_get_string(node));
		if (t == G_TYPE_BOOLEAN)
			return g_strdup(json_node_get_boolean(node) ? "true" : "false");
		if (t == G_TYPE_DOUBLE) {
			gchar buf[G_ASCII_DTOSTR_BUF_SIZE];

			return g_strdup(g_ascii_dtostr(buf, sizeof buf,
			                               json_node_get_double(node)));
		}
		return g_strdup_printf("%" G_GINT64_FORMAT, json_node_get_int(node));
	}
	if (JSON_NODE_HOLDS_ARRAY(node)) {
		JsonArray *a = json_node_get_array(node);
		GString *out = g_string_new(NULL);
		guint i;

		for (i = 0; i < json_array_get_length(a); i++) {
			JsonNode *e = json_array_get_element(a, i);
			g_autofree gchar *t = NULL;

			if (!JSON_NODE_HOLDS_VALUE(e)) {
				g_string_free(out, TRUE);
				return json_to_string(node, FALSE);
			}
			t = json_as_text(e);
			if (i > 0)
				g_string_append_c(out, '\n');
			g_string_append(out, t);
		}
		return g_string_free(out, FALSE);
	}
	return json_to_string(node, FALSE);
}

/* A variable's params, substituted: as a JSON node and as a table. */
/* An empty `{}' (json_node_init_object() takes its own reference) */
static JsonNode *
empty_object(void)
{
	g_autoptr(JsonObject) o = json_object_new();

	return json_node_init_object(json_node_alloc(), o);
}

static JsonNode *
build_params(
	const GabbrevVar *var,
	GHashTable       *vars,
	GHashTable       *table
){
	JsonNode *j;

	if (var->params == NULL)
		return empty_object();
	j = yaml_node_to_json_node(var->params);
	if (j == NULL || !JSON_NODE_HOLDS_OBJECT(j)) {
		if (j != NULL)
			json_node_unref(j);
		return empty_object();
	}
	if (var->inject_vars)
		json_substitute(j, vars);
	if (table != NULL) {
		JsonObject *o = json_node_get_object(j);
		g_autoptr(GList) members = json_object_get_members(o);
		GList *l;

		for (l = members; l != NULL; l = l->next)
			g_hash_table_insert(table, g_strdup(l->data),
			                    json_as_text(json_object_get_member(o, l->data)));
	}
	return j;
}

/* ── The render ──────────────────────────────────────────────────────── */

/**
 * gabbrev_render_new: (skip)
 * @info: what to render
 * @host: resolves and posts (see #GabbrevRenderHost)
 * @host_data: for @host
 * @done: (nullable): called once, when the render is finished or failed
 * @user_data: for @done
 *
 * Returns: (transfer full): a render, not started
 */
GabbrevRender *
gabbrev_render_new(
	const GabbrevRenderInfo *info,
	const GabbrevRenderHost *host,
	gpointer                 host_data,
	GabbrevRenderDone        done,
	gpointer                 user_data
){
	GabbrevRender *r;

	r = g_new0(GabbrevRender, 1);
	r->ref_count = 1;
	r->set = gabbrev_set_ref(info->set);
	r->match = gabbrev_match_ref(info->match);
	r->captures = info->captures != NULL ? g_hash_table_ref(info->captures)
	              : NULL;
	r->typed = g_strdup(info->typed);
	r->typed_case = info->typed_case;
	r->app_id = g_strdup(info->app_id);
	r->title = g_strdup(info->title);
	r->depth = info->depth;
	r->host = host;
	r->host_data = host_data;
	r->done = done;
	r->done_data = user_data;
	r->plan = g_ptr_array_new();
	r->vars = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	r->cursor_back = -1;
	return r;
}

/**
 * gabbrev_render_ref: (skip)
 * @render: a render
 *
 * Returns: (transfer full): @render
 */
GabbrevRender *
gabbrev_render_ref(GabbrevRender *render)
{
	g_atomic_int_inc(&render->ref_count);
	return render;
}

/**
 * gabbrev_render_unref: (skip)
 * @render: (transfer full): a render
 */
void
gabbrev_render_unref(GabbrevRender *render)
{
	if (render == NULL || !g_atomic_int_dec_and_test(&render->ref_count))
		return;
	if (render->pending != NULL) {
		gowl_gabbrev_request_cancel(render->pending);
		gowl_gabbrev_request_unref(render->pending);
	}
	if (render->child != NULL) {
		gabbrev_render_cancel(render->child);
		gabbrev_render_unref(render->child);
	}
	gabbrev_set_unref(render->set);
	gabbrev_match_unref(render->match);
	if (render->captures != NULL)
		g_hash_table_unref(render->captures);
	g_free(render->typed);
	g_free(render->app_id);
	g_free(render->title);
	g_ptr_array_unref(render->plan);
	g_hash_table_unref(render->vars);
	g_free(render->text);
	g_free(render->html);
	g_free(render->error);
	g_free(render);
}

static const GabbrevVar *
find_local(
	GabbrevRender *r,
	const gchar   *name
){
	guint i;

	for (i = 0; i < r->match->vars->len; i++) {
		const GabbrevVar *v = g_ptr_array_index(r->match->vars, i);

		if (g_strcmp0(v->name, name) == 0 && g_strcmp0(v->type, "global") != 0)
			return v;
	}
	return NULL;
}

/* @v, after its dependencies, once. */
static void
plan_add(
	GabbrevRender    *r,
	const GabbrevVar *v,
	GHashTable       *seen
){
	guint i;

	if (g_hash_table_contains(seen, v->name))
		return;
	g_hash_table_add(seen, v->name);
	for (i = 0; v->depends_on != NULL && v->depends_on[i] != NULL; i++) {
		const GabbrevVar *d = find_local(r, v->depends_on[i]);

		if (d == NULL)
			d = gabbrev_set_find_global(r->set, v->depends_on[i]);
		if (d != NULL)
			plan_add(r, d, seen);
	}
	g_ptr_array_add(r->plan, (gpointer)v);
}

static void
build_plan(GabbrevRender *r)
{
	g_autoptr(GHashTable) seen = g_hash_table_new(g_str_hash, g_str_equal);
	g_autoptr(GHashTable) wanted = g_hash_table_new_full(g_str_hash,
	                                                     g_str_equal, g_free,
	                                                     NULL);
	g_autoptr(GPtrArray) names = NULL;
	gboolean grew;
	guint i;
	guint j;

	/* What the text and the local params name */
	names = gabbrev_referenced_names(r->match->replace);
	for (i = 0; i < names->len; i++)
		g_hash_table_add(wanted, g_strdup(g_ptr_array_index(names, i)));
	g_ptr_array_unref(g_steal_pointer(&names));
	names = gabbrev_referenced_names(r->match->html);
	for (i = 0; i < names->len; i++)
		g_hash_table_add(wanted, g_strdup(g_ptr_array_index(names, i)));
	for (i = 0; i < r->match->vars->len; i++) {
		const GabbrevVar *v = g_ptr_array_index(r->match->vars, i);
		g_autofree gchar *pj = gabbrev_node_to_json(v->params);
		g_autoptr(GPtrArray) n = gabbrev_referenced_names(pj);

		for (j = 0; j < n->len; j++)
			g_hash_table_add(wanted, g_strdup(g_ptr_array_index(n, j)));
	}

	/* ... and what the globals named so far name, until nothing grows */
	do {
		grew = FALSE;
		for (i = 0; i < r->set->global_vars->len; i++) {
			const GabbrevVar *g = g_ptr_array_index(r->set->global_vars, i);
			g_autofree gchar *pj = NULL;
			g_autoptr(GPtrArray) n = NULL;

			if (!g_hash_table_contains(wanted, g->name))
				continue;
			pj = gabbrev_node_to_json(g->params);
			n = gabbrev_referenced_names(pj);
			for (j = 0; j < n->len; j++)
				if (!g_hash_table_contains(wanted, g_ptr_array_index(n, j))) {
					g_hash_table_add(wanted,
					                 g_strdup(g_ptr_array_index(n, j)));
					grew = TRUE;
				}
		}
	} while (grew);

	/* 1. globals first, unless shadowed or placed by `type: global' */
	for (i = 0; i < r->set->global_vars->len; i++) {
		const GabbrevVar *g = g_ptr_array_index(r->set->global_vars, i);
		gboolean placed = FALSE;

		if (!g_hash_table_contains(wanted, g->name) || find_local(r, g->name))
			continue;
		if (gabbrev_set_find_global(r->set, g->name) != g)
			continue;    /* a later definition wins */
		for (j = 0; j < r->match->vars->len; j++) {
			const GabbrevVar *v = g_ptr_array_index(r->match->vars, j);

			if (g_strcmp0(v->type, "global") == 0
			    && g_strcmp0(v->name, g->name) == 0)
				placed = TRUE;
		}
		if (!placed)
			plan_add(r, g, seen);
	}
	/* 2. the match's own */
	for (i = 0; i < r->match->vars->len; i++) {
		const GabbrevVar *v = g_ptr_array_index(r->match->vars, i);

		if (g_strcmp0(v->type, "global") == 0) {
			const GabbrevVar *g = gabbrev_set_find_global(r->set, v->name);

			if (g != NULL)
				plan_add(r, g, seen);
			else
				plan_add(r, v, seen);   /* fails with a reason */
			continue;
		}
		plan_add(r, v, seen);
	}
}

static void
finish(
	GabbrevRender *r,
	const gchar   *error
){
	GabbrevRenderDone done;

	if (r->finished)
		return;
	r->finished = TRUE;
	if (error != NULL) {
		r->error = g_strdup(error);
	} else {
		g_autoptr(GPtrArray) missing = g_ptr_array_new_with_free_func(g_free);
		g_autofree gchar *subst = NULL;
		g_autofree gchar *plain = NULL;

		subst = gabbrev_substitute(r->match->replace != NULL
		                           ? r->match->replace : "", r->vars, missing);
		plain = gabbrev_take_cursor_hint(subst, &r->cursor_back);
		if (r->match->propagate_case && r->typed_case != GABBREV_TYPED_AS_IS)
			r->text = gabbrev_apply_case(plain, r->typed_case,
			                             r->match->uppercase_style);
		else
			r->text = g_steal_pointer(&plain);
		if (r->match->html != NULL)
			r->html = gabbrev_substitute(r->match->html, r->vars, missing);
		if (missing->len > 0)
			g_message("gabbrev: %s: no variable `%s' (left as written)",
			          r->match->id, (gchar *)g_ptr_array_index(missing, 0));
	}
	done = r->done;
	r->done = NULL;
	/* The done notify may drop the last other reference (a job that is
	   over lets go of its render) */
	gabbrev_render_ref(r);
	if (done != NULL)
		done(r, r->done_data);
	gabbrev_render_unref(r);
}

/* Records @value for @var, and its fields when it is a JSON object
   (a form's answer, or anything with `json: true'). */
static void
store_value(
	GabbrevRender    *r,
	const GabbrevVar *var,
	const gchar      *value,
	gboolean          structured
){
	g_hash_table_replace(r->vars, g_strdup(var->name),
	                     g_strdup(value != NULL ? value : ""));
	if (!structured || value == NULL)
		return;
	{
		g_autoptr(JsonParser) p = json_parser_new();
		JsonObject *o;
		g_autoptr(GList) members = NULL;
		GList *l;

		if (!json_parser_load_from_data(p, value, -1, NULL)
		    || !JSON_NODE_HOLDS_OBJECT(json_parser_get_root(p)))
			return;
		o = json_node_get_object(json_parser_get_root(p));
		members = json_object_get_members(o);
		for (l = members; l != NULL; l = l->next)
			g_hash_table_replace(r->vars,
				g_strdup_printf("%s.%s", var->name, (gchar *)l->data),
				json_as_text(json_object_get_member(o, l->data)));
	}
}

static gboolean
var_is_structured(
	const GabbrevVar *var,
	GHashTable       *params
){
	const gchar *j = g_hash_table_lookup(params, "json");

	return g_strcmp0(var->type, "form") == 0
	       || g_strcmp0(j, "true") == 0 || g_strcmp0(j, "yes") == 0;
}

/* --- the types resolved right here --- */

static gchar *
resolve_date(
	GHashTable  *params,
	gchar      **error
){
	const gchar *format = g_hash_table_lookup(params, "format");
	const gchar *offset = g_hash_table_lookup(params, "offset");
	const gchar *tz = g_hash_table_lookup(params, "tz");
	g_autoptr(GTimeZone) zone = NULL;
	g_autoptr(GDateTime) now = NULL;
	g_autoptr(GDateTime) when = NULL;
	gchar *out;
	gint64 secs = 0;

	if (format == NULL || *format == '\0')
		format = "%Y-%m-%d %H:%M:%S";
	if (offset != NULL && *offset != '\0'
	    && !g_ascii_string_to_signed(offset, 10, G_MININT64 / 2,
	                                 G_MAXINT64 / 2, &secs, NULL)) {
		*error = g_strdup_printf("date: offset `%s' is not seconds", offset);
		return NULL;
	}
	if (tz != NULL && *tz != '\0') {
		zone = g_time_zone_new_identifier(tz);
		if (zone == NULL) {
			*error = g_strdup_printf("date: no time zone `%s'", tz);
			return NULL;
		}
	} else {
		zone = g_time_zone_new_local();
	}
	now = g_date_time_new_now(zone);
	when = g_date_time_add_seconds(now, (gdouble)secs);
	out = gabbrev_format_date(format, when);
	if (out == NULL)
		*error = g_strdup_printf("date: GLib cannot format `%s'", format);
	return out;
}

static gchar *
resolve_random(
	JsonNode  *params,
	gchar    **error
){
	JsonObject *o = json_node_get_object(params);
	JsonNode *choices = o != NULL ? json_object_get_member(o, "choices") : NULL;
	JsonArray *a;
	guint n;

	if (choices == NULL || !JSON_NODE_HOLDS_ARRAY(choices)
	    || (n = json_array_get_length(a = json_node_get_array(choices))) == 0) {
		*error = g_strdup("random: params.choices is a list of texts");
		return NULL;
	}
	return json_as_text(json_array_get_element(a,
	                    (guint)g_random_int_range(0, (gint32)n)));
}

static gchar *
resolve_env(
	GHashTable  *params,
	gchar      **error
){
	const gchar *name = g_hash_table_lookup(params, "name");
	const gchar *fallback = g_hash_table_lookup(params, "default");
	const gchar *v;

	if (name == NULL || *name == '\0') {
		*error = g_strdup("env: params.name names the variable");
		return NULL;
	}
	v = g_getenv(name);
	if (v != NULL)
		return g_strdup(v);
	if (fallback != NULL)
		return g_strdup(fallback);
	*error = g_strdup_printf("env: $%s is not set", name);
	return NULL;
}

/* --- nested `match' --- */

static void
child_done(
	GabbrevRender *child,
	gpointer       data
){
	GabbrevRender *r = data;
	const GabbrevVar *v;
	GabbrevRender *c;

	/* Finished inside gabbrev_render_start(), called from advance():
	   advance() sees it finished and takes the value itself. */
	if (r->in_advance || r->cancelled || r->child != child || r->finished)
		return;
	c = g_steal_pointer(&r->child);
	v = g_ptr_array_index(r->plan, r->step);
	if (c->error != NULL) {
		g_autofree gchar *msg = g_strdup_printf("%s: %s", v->name, c->error);

		gabbrev_render_unref(c);
		finish(r, msg);
		return;
	}
	store_value(r, v, c->text, FALSE);
	gabbrev_render_unref(c);
	r->step++;
	advance(r);
}

/* --- answers from elsewhere --- */

static void
request_done(
	GowlGabbrevRequest *request,
	gpointer            data
){
	GabbrevRender *r = data;

	/* Any thread.  The host takes both references. */
	r->host->post(r->host_data, gabbrev_render_ref(r),
	              gowl_gabbrev_request_ref(request));
}

/**
 * gabbrev_render_deliver: (skip)
 * @render: a render
 * @request: an answered request of this render's
 *
 * Continues the render with @request's answer.  On the render's thread
 * only; a request that is no longer the one it waits for is ignored.
 */
void
gabbrev_render_deliver(
	GabbrevRender      *render,
	GowlGabbrevRequest *request
){
	const GabbrevVar *v;
	const gchar *err;

	if (render->cancelled || render->finished || render->pending != request)
		return;
	render->pending = NULL;
	v = g_ptr_array_index(render->plan, render->step);
	err = gowl_gabbrev_request_get_error(request);
	if (err != NULL) {
		g_autofree gchar *msg = g_strdup_printf("%s (%s): %s", v->name,
		                                        v->type, err);

		gowl_gabbrev_request_unref(request);
		finish(render, msg);
		return;
	}
	store_value(render, v, gowl_gabbrev_request_get_value(request),
	            var_is_structured(v, gowl_gabbrev_request_get_params(request)));
	gowl_gabbrev_request_unref(request);
	render->step++;
	advance(render);
}

/* Resolves plan steps until one has to wait, or the plan is done. */
static void
advance(GabbrevRender *r)
{
	if (r->in_advance)
		return;
	r->in_advance = TRUE;
	gabbrev_render_ref(r);

	while (!r->finished && !r->cancelled && r->pending == NULL
	       && r->child == NULL && r->step < r->plan->len) {
		const GabbrevVar *v = g_ptr_array_index(r->plan, r->step);
		g_autoptr(GHashTable) params = NULL;
		g_autoptr(JsonNode) pj = NULL;
		g_autofree gchar *value = NULL;
		g_autofree gchar *error = NULL;
		gboolean handled = TRUE;

		params = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
		pj = build_params(v, r->vars, params);

		if (g_strcmp0(v->type, "echo") == 0) {
			const gchar *e = g_hash_table_lookup(params, "echo");

			value = g_strdup(e != NULL ? e : "");
		} else if (g_strcmp0(v->type, "date") == 0) {
			value = resolve_date(params, &error);
		} else if (g_strcmp0(v->type, "random") == 0) {
			value = resolve_random(pj, &error);
		} else if (g_strcmp0(v->type, "env") == 0) {
			value = resolve_env(params, &error);
		} else if (g_strcmp0(v->type, "uuid") == 0) {
			value = g_uuid_string_random();
		} else if (g_strcmp0(v->type, "global") == 0) {
			error = g_strdup_printf("no global variable `%s'", v->name);
		} else if (g_strcmp0(v->type, "match") == 0) {
			const gchar *t = g_hash_table_lookup(params, "trigger");
			GabbrevMatch *m = t != NULL
				? gabbrev_set_find_trigger(r->set, t) : NULL;
			GabbrevRenderInfo ci;

			if (m == NULL) {
				error = g_strdup_printf("match: no match with trigger `%s'",
				                        t != NULL ? t : "");
			} else if (r->depth + 1 >= GABBREV_MAX_RENDER_DEPTH) {
				error = g_strdup("match: nested too deep (a loop?)");
			} else {
				memset(&ci, 0, sizeof ci);
				ci.set = r->set;
				ci.match = m;
				ci.typed = r->typed;
				ci.app_id = r->app_id;
				ci.title = r->title;
				ci.depth = r->depth + 1;
				r->child = gabbrev_render_new(&ci, r->host, r->host_data,
				                              child_done, r);
				gabbrev_render_start(r->child);
				/* Done already: take the value now */
				if (r->child != NULL && r->child->finished) {
					GabbrevRender *c = g_steal_pointer(&r->child);

					if (c->error != NULL)
						error = g_strdup_printf("%s", c->error);
					else
						value = g_strdup(c->text);
					gabbrev_render_unref(c);
				} else {
					continue;   /* waiting for the child */
				}
			}
		} else {
			handled = FALSE;
		}

		if (handled) {
			if (error != NULL) {
				g_autofree gchar *msg = g_strdup_printf("%s: %s", v->name,
				                                        error);

				finish(r, msg);
				break;
			}
			store_value(r, v, value, var_is_structured(v, params));
			r->step++;
			continue;
		}

		/* Everything else is the host's */
		{
			GowlGabbrevRequestInfo info;
			g_autofree gchar *json = json_to_string(pj, FALSE);
			g_autoptr(GHashTable) snapshot = NULL;
			GHashTableIter it;
			gpointer k;
			gpointer val;
			GowlGabbrevRequest *req;

			snapshot = g_hash_table_new_full(g_str_hash, g_str_equal,
			                                 g_free, g_free);
			g_hash_table_iter_init(&it, r->vars);
			while (g_hash_table_iter_next(&it, &k, &val))
				g_hash_table_insert(snapshot, g_strdup(k), g_strdup(val));

			memset(&info, 0, sizeof info);
			info.var_name = v->name;
			info.var_type = v->type;
			info.params = params;
			info.params_json = json;
			info.vars = snapshot;
			info.trigger = r->typed;
			info.app_id = r->app_id;
			info.title = r->title;
			req = gowl_gabbrev_request_new(&info, request_done,
			                               gabbrev_render_ref(r),
			                               (GDestroyNotify)gabbrev_render_unref);
			r->pending = req;
			if (!r->host->resolve(r->host_data, r, v, req)) {
				g_autofree gchar *msg = g_strdup_printf(
					"%s: no variable type `%s'", v->name, v->type);

				gowl_gabbrev_request_cancel(req);
				r->pending = NULL;
				gowl_gabbrev_request_unref(req);
				finish(r, msg);
				break;
			}
		}
	}
	if (!r->finished && !r->cancelled && r->pending == NULL
	    && r->child == NULL && r->step >= r->plan->len)
		finish(r, NULL);

	r->in_advance = FALSE;
	gabbrev_render_unref(r);
}

/**
 * gabbrev_render_start: (skip)
 * @render: a render
 *
 * Plans and starts resolving.  The done notify may run before this
 * returns, when nothing had to wait.
 */
void
gabbrev_render_start(GabbrevRender *render)
{
	if (render->started)
		return;
	render->started = TRUE;
	if (render->captures != NULL) {
		GHashTableIter it;
		gpointer k;
		gpointer v;

		g_hash_table_iter_init(&it, render->captures);
		while (g_hash_table_iter_next(&it, &k, &v))
			g_hash_table_replace(render->vars, g_strdup(k), g_strdup(v));
	}
	build_plan(render);
	advance(render);
}

/**
 * gabbrev_render_cancel: (skip)
 * @render: a render
 *
 * Stops waiting: an outstanding request is cancelled and the done
 * notify will not be called.
 */
void
gabbrev_render_cancel(GabbrevRender *render)
{
	if (render->cancelled)
		return;
	render->cancelled = TRUE;
	render->done = NULL;
	if (render->pending != NULL) {
		gowl_gabbrev_request_cancel(render->pending);
		gowl_gabbrev_request_unref(g_steal_pointer(&render->pending));
	}
	if (render->child != NULL) {
		gabbrev_render_cancel(render->child);
		gabbrev_render_unref(g_steal_pointer(&render->child));
	}
}

/**
 * gabbrev_render_is_done: (skip)
 * @render: a render
 *
 * Returns: %TRUE once finished or failed
 */
gboolean
gabbrev_render_is_done(GabbrevRender *render)
{
	return render->finished;
}

/**
 * gabbrev_render_get_text: (skip)
 * @render: a finished render
 *
 * Returns: (transfer none) (nullable): the text to type, %NULL on error
 */
const gchar *
gabbrev_render_get_text(GabbrevRender *render)
{
	return render->text;
}

/**
 * gabbrev_render_get_html: (skip)
 * @render: a finished render
 *
 * Returns: (transfer none) (nullable): the HTML to paste, if the match
 *   has some
 */
const gchar *
gabbrev_render_get_html(GabbrevRender *render)
{
	return render->html;
}

/**
 * gabbrev_render_get_error: (skip)
 * @render: a finished render
 *
 * Returns: (transfer none) (nullable): why it failed
 */
const gchar *
gabbrev_render_get_error(GabbrevRender *render)
{
	return render->error;
}

/**
 * gabbrev_render_get_cursor_back: (skip)
 * @render: a finished render
 *
 * Returns: how many characters left of the end the cursor goes, -1 for
 *   the end (no hint)
 */
gint
gabbrev_render_get_cursor_back(GabbrevRender *render)
{
	return render->cursor_back;
}

/**
 * gabbrev_render_get_vars: (skip)
 * @render: a render
 *
 * Returns: (transfer none): the values so far
 */
GHashTable *
gabbrev_render_get_vars(GabbrevRender *render)
{
	return render->vars;
}

/**
 * gabbrev_render_get_match: (skip)
 * @render: a render
 *
 * Returns: (transfer none): the match being rendered
 */
GabbrevMatch *
gabbrev_render_get_match(GabbrevRender *render)
{
	return render->match;
}

/**
 * gabbrev_render_get_pending_type: (skip)
 * @render: a render
 *
 * Returns: (transfer none) (nullable): the type of the variable being
 *   waited for, for status
 */
const gchar *
gabbrev_render_get_pending_type(GabbrevRender *render)
{
	if (render->child != NULL)
		return gabbrev_render_get_pending_type(render->child);
	if (render->pending != NULL)
		return gowl_gabbrev_request_get_var_type(render->pending);
	return NULL;
}
