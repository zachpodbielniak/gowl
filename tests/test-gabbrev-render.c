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
 * test-gabbrev-render.c - Variables, in order, then the replacement.
 *
 * The renderer with a fake host.  The host's own types:
 *
 *   `now'     answered inside resolve(), with params.value
 *   `thread'  answered from another thread, with params.value, which is
 *             how Lisp under cmacs and a compile worker answer
 *   `fail'    answered with an error
 *   `never'   never answered (the job's timeout is the module's)
 *
 * Answers are POSTED to a queue the test drains, as the module drains
 * its eventfd queue on the event loop.
 */

#include <glib.h>
#include <string.h>

#include "gowl.h"
#include "../modules/gabbrev/gowl-gabbrev-model.c"
#include "../modules/gabbrev/gowl-gabbrev-builtin.c"
#include "../modules/gabbrev/gowl-gabbrev-engine.c"
#include "../modules/gabbrev/gowl-gabbrev-render.c"

typedef struct {
	GAsyncQueue *posted;      /* Delivery* */
	GPtrArray   *seen;        /* GowlGabbrevRequest* refs, every request */
	gboolean     done;
} Host;

typedef struct {
	GabbrevRender      *render;
	GowlGabbrevRequest *request;
} Delivery;

static gpointer
answer_thread(gpointer data)
{
	GowlGabbrevRequest *req = data;

	g_usleep(2000);
	gowl_gabbrev_request_return(req,
		gowl_gabbrev_request_get_param(req, "value"));
	gowl_gabbrev_request_unref(req);
	return NULL;
}

static gboolean
fake_resolve(
	gpointer            host_data,
	GabbrevRender      *render,
	const GabbrevVar   *var,
	GowlGabbrevRequest *req
){
	Host *h = host_data;

	(void)render;
	g_ptr_array_add(h->seen, gowl_gabbrev_request_ref(req));
	if (g_strcmp0(var->type, "now") == 0) {
		gowl_gabbrev_request_return(req,
			gowl_gabbrev_request_get_param(req, "value"));
		return TRUE;
	}
	if (g_strcmp0(var->type, "thread") == 0) {
		g_thread_unref(g_thread_new("answer", answer_thread,
		                            gowl_gabbrev_request_ref(req)));
		return TRUE;
	}
	if (g_strcmp0(var->type, "fail") == 0) {
		gowl_gabbrev_request_return_error(req, "it broke");
		return TRUE;
	}
	if (g_strcmp0(var->type, "never") == 0)
		return TRUE;
	return FALSE;
}

static void
fake_post(
	gpointer            host_data,
	GabbrevRender      *render,
	GowlGabbrevRequest *request
){
	Host *h = host_data;
	Delivery *d = g_new0(Delivery, 1);

	d->render = render;
	d->request = request;
	g_async_queue_push(h->posted, d);
}

static const GabbrevRenderHost fake_host = { fake_resolve, fake_post };

static void
on_done(
	GabbrevRender *render,
	gpointer       data
){
	Host *h = data;

	(void)render;
	h->done = TRUE;
}

/* Renders @trigger of @yaml to the end; the result is the render. */
static GabbrevRender *
render_full(
	const gchar *yaml,
	const gchar *trigger,
	const gchar *typed,
	Host        *h
){
	g_autoptr(GabbrevLoader) l = gabbrev_loader_new();
	g_autoptr(GabbrevSet) set = NULL;
	GabbrevRenderInfo info;
	GabbrevRender *r;
	gint64 deadline;

	g_assert_true(gabbrev_loader_add_text(l, yaml, NULL, GABBREV_ORIGIN_RUNTIME));
	set = gabbrev_loader_finish(l);
	g_assert_cmpuint(set->problems->len, ==, 0);

	memset(&info, 0, sizeof info);
	info.set = set;
	info.match = gabbrev_set_find_id(set, trigger);
	g_assert_nonnull(info.match);
	info.typed = typed;
	info.typed_case = typed != NULL ? gabbrev_typed_case(typed)
	                  : GABBREV_TYPED_AS_IS;
	info.app_id = "org.example.App";
	info.title = "A window";
	h->posted = g_async_queue_new();
	h->seen = g_ptr_array_new_with_free_func(
		(GDestroyNotify)gowl_gabbrev_request_unref);
	h->done = FALSE;
	r = gabbrev_render_new(&info, &fake_host, h, on_done, h);
	gabbrev_render_start(r);

	/* The event loop: deliver what was posted until done */
	deadline = g_get_monotonic_time() + 2 * G_USEC_PER_SEC;
	while (!h->done && g_get_monotonic_time() < deadline) {
		Delivery *d = g_async_queue_timeout_pop(h->posted, 10000);

		if (d == NULL)
			continue;
		gabbrev_render_deliver(d->render, d->request);
		gabbrev_render_unref(d->render);
		gowl_gabbrev_request_unref(d->request);
		g_free(d);
	}
	return r;
}

static void
host_clear(Host *h)
{
	Delivery *d;

	while ((d = g_async_queue_try_pop(h->posted)) != NULL) {
		gabbrev_render_unref(d->render);
		gowl_gabbrev_request_unref(d->request);
		g_free(d);
	}
	g_async_queue_unref(h->posted);
	g_ptr_array_unref(h->seen);
}

/* --- pure helpers --- */

static void
test_format_date(void)
{
	g_autoptr(GTimeZone) utc = g_time_zone_new_utc();
	g_autoptr(GDateTime) d = g_date_time_new(utc, 2026, 10, 19, 14, 5, 9.25);
	g_autoptr(GDateTime) d1 = g_date_time_new(utc, 2026, 10, 1, 0, 0, 0);
	g_autoptr(GDateTime) d2 = g_date_time_new(utc, 2026, 10, 22, 0, 0, 0);
	g_autoptr(GDateTime) d3 = g_date_time_new(utc, 2026, 10, 13, 0, 0, 0);
	g_autofree gchar *iso = gabbrev_format_date("%Y-%m-%d", d);
	g_autofree gchar *human = gabbrev_format_date("%B %-d%o, %Y", d);
	g_autofree gchar *machine = gabbrev_format_date("%Y%m%d", d);
	g_autofree gchar *now = gabbrev_format_date("%Y%m%d%H%M%S", d);
	g_autofree gchar *st = gabbrev_format_date("%-d%o", d1);
	g_autofree gchar *nd = gabbrev_format_date("%-d%o", d2);
	g_autofree gchar *th = gabbrev_format_date("%-d%o", d3);
	g_autofree gchar *ms = gabbrev_format_date("%S%.3f", d);
	g_autofree gchar *pct = gabbrev_format_date("100%%o", d);

	g_assert_cmpstr(iso, ==, "2026-10-19");
	g_assert_cmpstr(human, ==, "October 19th, 2026");
	g_assert_cmpstr(machine, ==, "20261019");
	g_assert_cmpstr(now, ==, "20261019140509");
	g_assert_cmpstr(st, ==, "1st");
	g_assert_cmpstr(nd, ==, "22nd");
	g_assert_cmpstr(th, ==, "13th");
	g_assert_cmpstr(ms, ==, "09250");
	g_assert_cmpstr(pct, ==, "100%o");
}

static void
test_substitute(void)
{
	g_autoptr(GHashTable) vars = g_hash_table_new(g_str_hash, g_str_equal);
	g_autoptr(GPtrArray) missing = g_ptr_array_new_with_free_func(g_free);
	g_autofree gchar *out = NULL;
	g_autoptr(GPtrArray) names = NULL;
	g_autofree gchar *plain = NULL;
	gint back = 0;

	g_hash_table_insert(vars, "a", "1");
	g_hash_table_insert(vars, "f.x", "2");
	out = gabbrev_substitute("{{a}} {{ f.x }} {{nope}}", vars, missing);
	g_assert_cmpstr(out, ==, "1 2 {{nope}}");
	g_assert_cmpuint(missing->len, ==, 1);
	names = gabbrev_referenced_names("{{a}} {{f.x}} {{a}}");
	g_assert_cmpuint(names->len, ==, 2);
	g_assert_cmpstr(g_ptr_array_index(names, 1), ==, "f");
	plain = gabbrev_take_cursor_hint("fn($|$) {}", &back);
	g_assert_cmpstr(plain, ==, "fn() {}");
	g_assert_cmpint(back, ==, 4);
}

/* --- renders --- */

static void
test_static_and_builtin_types(void)
{
	Host h;
	g_autoptr(GabbrevRender) r = render_full(
		"matches:\n"
		"  - trigger: ';x'\n"
		"    replace: '{{e}}/{{u}}/{{env}}/{{d}}'\n"
		"    vars:\n"
		"      - {name: e, type: echo, params: {echo: hi}}\n"
		"      - {name: u, type: uuid}\n"
		"      - {name: env, type: env, params: {name: GABBREV_TEST_ENV}}\n"
		"      - {name: d, type: date, params: {format: '%Y', offset: 0, tz: UTC}}\n",
		";x", NULL, &h);
	g_auto(GStrv) parts = NULL;

	g_assert_true(gabbrev_render_is_done(r));
	g_assert_null(gabbrev_render_get_error(r));
	parts = g_strsplit(gabbrev_render_get_text(r), "/", -1);
	g_assert_cmpstr(parts[0], ==, "hi");
	g_assert_true(g_uuid_string_is_valid(parts[1]));
	g_assert_cmpstr(parts[2], ==, "set-by-test");
	g_assert_cmpuint(strlen(parts[3]), ==, 4);
	g_assert_cmpuint(h.seen->len, ==, 0);   /* nothing went to the host */
	host_clear(&h);
}

/* Host types, posted back from this thread and another one, in order;
   each sees the values before it in its params */
static void
test_host_types(void)
{
	Host h;
	g_autoptr(GabbrevRender) r = render_full(
		"matches:\n"
		"  - trigger: ';x'\n"
		"    replace: '{{a}}-{{b}}'\n"
		"    vars:\n"
		"      - {name: a, type: thread, params: {value: one}}\n"
		"      - {name: b, type: now, params: {value: 'two after {{a}}'}}\n",
		";x", NULL, &h);
	GowlGabbrevRequest *second;

	g_assert_cmpstr(gabbrev_render_get_text(r), ==, "one-two after one");
	g_assert_cmpuint(h.seen->len, ==, 2);
	second = g_ptr_array_index(h.seen, 1);
	g_assert_cmpstr(gowl_gabbrev_request_get_var(second, "a"), ==, "one");
	g_assert_cmpstr(gowl_gabbrev_request_get_app_id(second), ==,
	                "org.example.App");
	g_assert_cmpstr(gowl_gabbrev_request_get_params_json(second), ==,
	                "{\"value\":\"two after one\"}");
	host_clear(&h);
}

/* inject_vars: false leaves params as written */
static void
test_inject_vars_off(void)
{
	Host h;
	g_autoptr(GabbrevRender) r = render_full(
		"matches:\n"
		"  - trigger: ';x'\n"
		"    replace: '{{b}}'\n"
		"    vars:\n"
		"      - {name: a, type: echo, params: {echo: A}}\n"
		"      - {name: b, type: now, inject_vars: false, "
		"params: {value: 'raw {{a}}'}}\n",
		";x", NULL, &h);

	g_assert_cmpstr(gabbrev_render_get_text(r), ==, "raw {{a}}");
	host_clear(&h);
}

static void
test_failure(void)
{
	Host h;
	g_autoptr(GabbrevRender) r = render_full(
		"matches:\n"
		"  - trigger: ';x'\n"
		"    replace: '{{a}}'\n"
		"    vars: [{name: a, type: fail}]\n",
		";x", NULL, &h);

	g_assert_true(gabbrev_render_is_done(r));
	g_assert_null(gabbrev_render_get_text(r));
	g_assert_nonnull(strstr(gabbrev_render_get_error(r), "it broke"));
	host_clear(&h);
}

static void
test_unknown_type(void)
{
	Host h;
	g_autoptr(GabbrevRender) r = render_full(
		"matches:\n"
		"  - trigger: ';x'\n"
		"    replace: '{{a}}'\n"
		"    vars: [{name: a, type: telepathy}]\n",
		";x", NULL, &h);

	g_assert_nonnull(strstr(gabbrev_render_get_error(r), "telepathy"));
	host_clear(&h);
}

/* Globals: the ones the text names, first; `type: global' moves one;
   depends_on goes first; a local shadows a global */
static void
test_globals(void)
{
	Host h;
	g_autoptr(GabbrevRender) r = render_full(
		"global_vars:\n"
		"  - {name: g1, type: now, params: {value: G1}}\n"
		"  - {name: g2, type: now, params: {value: 'G2({{g1}})'}}\n"
		"  - {name: unused, type: fail}\n"
		"  - {name: shadow, type: now, params: {value: global}}\n"
		"matches:\n"
		"  - trigger: ';x'\n"
		"    replace: '{{g2}} {{shadow}} {{late}}'\n"
		"    vars:\n"
		"      - {name: shadow, type: echo, params: {echo: local}}\n"
		"      - {name: late, type: now, params: {value: L}, depends_on: [first]}\n"
		"      - {name: first, type: echo, params: {echo: F}}\n",
		";x", NULL, &h);
	guint i;

	g_assert_null(gabbrev_render_get_error(r));
	g_assert_cmpstr(gabbrev_render_get_text(r), ==, "G2(G1) local L");
	/* `unused' was never asked for; g1 (named by g2) was, before g2 */
	for (i = 0; i < h.seen->len; i++)
		g_assert_cmpstr(gowl_gabbrev_request_get_var_name(
			g_ptr_array_index(h.seen, i)), !=, "unused");
	g_assert_cmpstr(gowl_gabbrev_request_get_var_name(
		g_ptr_array_index(h.seen, 0)), ==, "g1");
	host_clear(&h);
}

/* A `match' variable renders another match; a loop is stopped */
static void
test_nested_match(void)
{
	Host h;
	g_autoptr(GabbrevRender) r = render_full(
		"matches:\n"
		"  - {trigger: ';inner', replace: 'in({{v}})', "
		"vars: [{name: v, type: thread, params: {value: deep}}]}\n"
		"  - {trigger: ';outer', replace: 'out[{{m}}]', "
		"vars: [{name: m, type: match, params: {trigger: ';inner'}}]}\n"
		"  - {trigger: ';loop', replace: '{{m}}', "
		"vars: [{name: m, type: match, params: {trigger: ';loop'}}]}\n",
		";outer", NULL, &h);
	GabbrevRender *loop;
	Host h2;

	g_assert_cmpstr(gabbrev_render_get_text(r), ==, "out[in(deep)]");
	host_clear(&h);

	loop = render_full(
		"matches:\n"
		"  - {trigger: ';loop', replace: '{{m}}', "
		"vars: [{name: m, type: match, params: {trigger: ';loop'}}]}\n",
		";loop", NULL, &h2);
	g_assert_nonnull(strstr(gabbrev_render_get_error(loop), "deep"));
	gabbrev_render_unref(loop);
	host_clear(&h2);
}

/* Regex groups, the cursor hint, propagate_case, a form's fields */
static void
test_captures_case_cursor_form(void)
{
	Host h;
	g_autoptr(GabbrevRender) r = render_full(
		"matches:\n"
		"  - {regex: ':hi(?P<who>\\w+)\\.', replace: 'hello {{who}}$|$!'}\n",
		":hi(?P<who>\\w+)\\.", NULL, &h);

	/* No captures given: the placeholder stays as written */
	g_assert_cmpstr(gabbrev_render_get_text(r), ==, "hello {{who}}!");
	g_assert_cmpint(gabbrev_render_get_cursor_back(r), ==, 1);
	host_clear(&h);

	{
		Host hc;
		g_autoptr(GabbrevRender) rc = render_full(
			"matches:\n"
			"  - {trigger: ';hello', replace: 'hello world', "
			"propagate_case: true, uppercase_style: capitalize_words}\n",
			";hello", ";Hello", &hc);

		g_assert_cmpstr(gabbrev_render_get_text(rc), ==, "Hello World");
		host_clear(&hc);
	}
	{
		Host hf;
		g_autoptr(GabbrevRender) rf = render_full(
			"matches:\n"
			"  - {trigger: ';f', replace: '{{f.a}}+{{f.b}}', vars: "
			"[{name: f, type: now, params: {json: true, "
			"value: '{\"a\": \"x\", \"b\": 2}'}}]}\n",
			";f", NULL, &hf);

		g_assert_cmpstr(gabbrev_render_get_text(rf), ==, "x+2");
		host_clear(&hf);
	}
}

/* Cancelled while waiting: no done notify, and a late answer is
   dropped (the request no longer reaches the render) */
static void
test_cancel(void)
{
	g_autoptr(GabbrevLoader) l = gabbrev_loader_new();
	g_autoptr(GabbrevSet) set = NULL;
	GabbrevRenderInfo info;
	GabbrevRender *r;
	GowlGabbrevRequest *req;
	Host h;

	g_assert_true(gabbrev_loader_add_text(l,
		"matches: [{trigger: ';x', replace: '{{a}}', "
		"vars: [{name: a, type: never}]}]", NULL, GABBREV_ORIGIN_RUNTIME));
	set = gabbrev_loader_finish(l);
	memset(&info, 0, sizeof info);
	info.set = set;
	info.match = gabbrev_set_find_trigger(set, ";x");
	h.posted = g_async_queue_new();
	h.seen = g_ptr_array_new_with_free_func(
		(GDestroyNotify)gowl_gabbrev_request_unref);
	h.done = FALSE;
	r = gabbrev_render_new(&info, &fake_host, &h, on_done, &h);
	gabbrev_render_start(r);
	g_assert_cmpuint(h.seen->len, ==, 1);
	req = g_ptr_array_index(h.seen, 0);
	g_assert_cmpstr(gabbrev_render_get_pending_type(r), ==, "never");

	gabbrev_render_cancel(r);
	gabbrev_render_unref(r);
	g_assert_false(gowl_gabbrev_request_return(req, "late"));
	g_assert_false(gowl_gabbrev_request_return_by_id(
		gowl_gabbrev_request_get_id(req), "late", NULL));
	g_assert_null(g_async_queue_try_pop(h.posted));
	g_assert_false(h.done);
	host_clear(&h);
}

/* The libgowl registry and answering by id */
static gchar *
registered_func(
	GowlGabbrevRequest *req,
	gpointer            data
){
	return g_strdup_printf("%s:%s", (gchar *)data,
	                       gowl_gabbrev_request_get_var_name(req));
}

static void
test_registry(void)
{
	g_autoptr(GowlGabbrevHandle) h = NULL;
	g_auto(GStrv) names = NULL;
	GowlGabbrevRequestInfo info;
	g_autoptr(GowlGabbrevRequest) req = NULL;
	g_autofree gchar *v = NULL;
	guint gen;

	gen = gowl_gabbrev_get_generation();
	gowl_gabbrev_register_func("greet", registered_func, g_strdup("hi"), g_free);
	g_assert_cmpuint(gowl_gabbrev_get_generation(), !=, gen);
	names = gowl_gabbrev_list_funcs();
	g_assert_true(g_strv_contains((const gchar * const *)names, "greet"));

	memset(&info, 0, sizeof info);
	info.var_name = "who";
	info.var_type = "c";
	req = gowl_gabbrev_request_new(&info, NULL, NULL, NULL);
	h = gowl_gabbrev_lookup_func("greet");
	g_assert_nonnull(h);
	v = gowl_gabbrev_handle_call_func(h, req);
	g_assert_cmpstr(v, ==, "hi:who");
	g_assert_true(gowl_gabbrev_unregister_func("greet"));
	g_assert_null(gowl_gabbrev_lookup_func("greet"));

	/* answered once, by id, then never again */
	g_assert_true(gowl_gabbrev_request_return_by_id(
		gowl_gabbrev_request_get_id(req), "first", NULL));
	g_assert_false(gowl_gabbrev_request_return(req, "second"));
	g_assert_cmpstr(gowl_gabbrev_request_get_value(req), ==, "first");

	/* the process-wide sources */
	gowl_gabbrev_clear_sources();
	gowl_gabbrev_add_file("~/nowhere/gabbrev.yml");
	g_assert_true(gowl_gabbrev_add_yaml("matches: []", NULL));
	g_assert_false(gowl_gabbrev_add_yaml("matches: [", NULL));
	{
		g_autoptr(GPtrArray) s = gowl_gabbrev_dup_sources();

		g_assert_cmpuint(s->len, ==, 2);
	}
	gowl_gabbrev_clear_sources();
}

int
main(int argc, char **argv)
{
	g_setenv("GABBREV_TEST_ENV", "set-by-test", TRUE);
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/gabbrev/render/format-date", test_format_date);
	g_test_add_func("/gabbrev/render/substitute", test_substitute);
	g_test_add_func("/gabbrev/render/builtin-types", test_static_and_builtin_types);
	g_test_add_func("/gabbrev/render/host-types", test_host_types);
	g_test_add_func("/gabbrev/render/inject-vars-off", test_inject_vars_off);
	g_test_add_func("/gabbrev/render/failure", test_failure);
	g_test_add_func("/gabbrev/render/unknown-type", test_unknown_type);
	g_test_add_func("/gabbrev/render/globals", test_globals);
	g_test_add_func("/gabbrev/render/nested-match", test_nested_match);
	g_test_add_func("/gabbrev/render/captures-case-cursor-form",
	                test_captures_case_cursor_form);
	g_test_add_func("/gabbrev/render/cancel", test_cancel);
	g_test_add_func("/gabbrev/render/registry", test_registry);
	return g_test_run();
}
