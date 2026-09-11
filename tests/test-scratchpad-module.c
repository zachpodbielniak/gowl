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
 * The scratchpad module, against the real .so.
 *
 * It runs against a compositor that was never started: no display, no
 * scene, no seat.  That covers everything the module decides -- which
 * windows are members, what each command answers, how its settings parse,
 * that switching it off gives every window back -- while the calls that
 * need a scene (showing a window is one) do nothing here.  The panel's
 * shape is covered by test-overlay-layout and the adopt/release contract
 * by test-overlay-adopt, which leaves only how it looks for a live session.
 */

#include <gmodule.h>
#include <string.h>

#include "core/gowl-core-private.h"
#include "module/gowl-module.h"
#include "interfaces/gowl-ipc-handler.h"
#include "interfaces/gowl-startup-handler.h"
#include "interfaces/gowl-scratchpad-handler.h"

#ifndef GOWL_TEST_SCRATCHPAD_MODULE
#define GOWL_TEST_SCRATCHPAD_MODULE "build/release/modules/scratchpad.so"
#endif

/* Every test body starts with this: a module that failed to load was
 * already reported as a skip by the fixture. */
#define NEED_MODULE(f) \
	G_STMT_START { if ((f)->module == NULL) return; } G_STMT_END

/* Run EXPR for a reply, compare it and free it. */
#define ASSERT_REPLY(expr, expected) \
	G_STMT_START { \
		gchar *reply_ = (expr); \
		g_assert_cmpstr(reply_, ==, (expected)); \
		g_free(reply_); \
	} G_STMT_END

#define ASSERT_REPLY_HAS(expr, needle) \
	G_STMT_START { \
		gchar *reply_ = (expr); \
		g_assert_nonnull(reply_); \
		if (strstr(reply_, (needle)) == NULL) \
			g_error("reply \"%s\" lacks \"%s\"", reply_, (needle)); \
		g_free(reply_); \
	} G_STMT_END

/*
 * gowl builds with G_LOG_USE_STRUCTURED, so a module's g_warning() goes
 * through the log writer: g_test_expect_message() never sees it, and
 * GTest's abort on warnings fires instead.  A writer is the only place to
 * catch the one warning a test expects.  It can be installed once per
 * process, so it stays, and falls through to the default whenever no
 * warning is expected -- the arrangement test-config.c uses.
 */
static const gchar *expected_warning = NULL;
static gboolean     saw_expected_warning = FALSE;

static GLogWriterOutput
expect_warning_writer(
	GLogLevelFlags   log_level,
	const GLogField *fields,
	gsize            n_fields,
	gpointer         user_data
){
	gsize i;

	(void)user_data;
	if ((log_level & G_LOG_LEVEL_WARNING) && expected_warning != NULL) {
		for (i = 0; i < n_fields; i++) {
			if (g_strcmp0(fields[i].key, "MESSAGE") == 0
			    && fields[i].value != NULL
			    && strstr((const gchar *)fields[i].value,
			              expected_warning) != NULL) {
				saw_expected_warning = TRUE;
				return G_LOG_WRITER_HANDLED;
			}
		}
	}
	return g_log_writer_default(log_level, fields, n_fields, NULL);
}

static void
expect_warning(const gchar *substring)
{
	static gboolean installed = FALSE;

	if (!installed) {
		g_log_set_writer_func(expect_warning_writer, NULL, NULL);
		installed = TRUE;
	}
	expected_warning = substring;
	saw_expected_warning = FALSE;
}

typedef struct {
	GowlModule     *module;
	GowlCompositor *comp;
	GowlMonitor    *mon;
	GowlClient     *a;
	GowlClient     *b;
	GowlClient     *c;
} Fixture;

static GType
scratchpad_type(void)
{
	static GType type = 0;
	GModule *module;
	gpointer symbol;
	GType (*register_fn)(void);

	if (type != 0)
		return type;
	module = g_module_open(GOWL_TEST_SCRATCHPAD_MODULE, G_MODULE_BIND_LAZY);
	if (module == NULL)
		return 0;
	g_module_make_resident(module);
	if (!g_module_symbol(module, "gowl_module_register", &symbol))
		return 0;
	register_fn = (GType (*)(void))symbol;
	type = register_fn();
	return type;
}

static GowlClient *
client_on(GowlMonitor *mon)
{
	GowlClient *c;

	c = gowl_client_new();
	c->mon = mon;
	c->tags = 1;
	return c;
}

/* Three windows on tag 1, `a' holding the keyboard, and the module
 * started against them. */
static void
fixture_setup(Fixture *f, gconstpointer data)
{
	GType type;

	(void)data;
	memset(f, 0, sizeof(*f));
	type = scratchpad_type();
	if (type == 0) {
		g_test_skip("scratchpad.so did not load");
		return;
	}

	f->comp = gowl_compositor_new();
	f->mon = g_object_new(GOWL_TYPE_MONITOR, NULL);
	f->mon->tagset[f->mon->seltags] = 1;
	f->comp->selmon = f->mon;
	f->comp->monitors = g_list_append(NULL, f->mon);

	f->a = client_on(f->mon);
	f->b = client_on(f->mon);
	f->c = client_on(f->mon);
	f->comp->clients = g_list_append(f->comp->clients, f->a);
	f->comp->clients = g_list_append(f->comp->clients, f->b);
	f->comp->clients = g_list_append(f->comp->clients, f->c);
	f->comp->fstack = g_list_copy(f->comp->clients);

	f->module = (GowlModule *)g_object_new(type, NULL);
	g_assert_true(gowl_module_activate(f->module));
	gowl_startup_handler_on_startup(GOWL_STARTUP_HANDLER(f->module),
	                                f->comp);
}

static void
fixture_teardown(Fixture *f, gconstpointer data)
{
	(void)data;
	if (f->comp == NULL)
		return;
	/* The module first, while the windows it may release still exist. */
	g_clear_object(&f->module);
	g_clear_pointer(&f->comp->fstack, g_list_free);
	g_clear_pointer(&f->comp->clients, g_list_free);
	g_clear_pointer(&f->comp->monitors, g_list_free);
	f->comp->selmon = NULL;
	f->a->mon = NULL;
	f->b->mon = NULL;
	f->c->mon = NULL;
	g_object_unref(f->a);
	g_object_unref(f->b);
	g_object_unref(f->c);
	g_object_unref(f->comp);
	g_object_unref(f->mon);
}

/* One command, as a keybind or the IPC socket would send it. */
static gchar *
run(Fixture *f, const gchar *command, const gchar *args)
{
	return gowl_ipc_handler_handle_command(GOWL_IPC_HANDLER(f->module),
	                                       command, args);
}

static gchar *
status(Fixture *f)
{
	return run(f, "scratchpad-status", NULL);
}

static gchar *
id_of(GowlClient *c)
{
	return g_strdup_printf("%u", gowl_client_get_id(c));
}

static gboolean
is_member(Fixture *f, GowlClient *c)
{
	return gowl_scratchpad_handler_is_scratchpad(
		GOWL_SCRATCHPAD_HANDLER(f->module), c);
}

/* Configure one setting, the way a YAML modules: entry or cmacs's
 * gowl-configure-module hands it over. */
static void
configure(Fixture *f, const gchar *key, const gchar *value)
{
	GHashTable *settings;

	settings = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	g_hash_table_insert(settings, g_strdup(key), g_strdup(value));
	gowl_module_configure(f->module, settings);
	g_hash_table_unref(settings);
}

/* The focused window joins as a hidden overlay on no tag, and what counts
 * as focused is now the next window. */
static void
test_add_focused(Fixture *f, gconstpointer data)
{
	g_autofree gchar *expect = NULL;

	(void)data;
	NEED_MODULE(f);
	ASSERT_REPLY(run(f, "scratchpad-add", NULL), "OK added 1");
	g_assert_true(f->a->isoverlay);
	g_assert_false(f->a->overlay_visible);
	g_assert_cmpuint(f->a->overlay_group, !=, 0);
	g_assert_cmpuint(f->a->tags, ==, 0);
	g_assert_true(is_member(f, f->a));
	g_assert_false(is_member(f, f->b));
	g_assert_true(gowl_compositor_get_focused_client(f->comp) == f->b);

	expect = g_strdup_printf("OK visible=0 count=1 members=%u width-pct=1 "
	                         "height-pct=0.666667 width=0 height=0 gap=0",
	                         gowl_client_get_id(f->a));
	ASSERT_REPLY(status(f), expect);
}

/* By id, and every way a window can be refused. */
static void
test_add_refusals(Fixture *f, gconstpointer data)
{
	g_autofree gchar *b_id = NULL;
	g_autofree gchar *c_id = NULL;

	(void)data;
	NEED_MODULE(f);
	b_id = id_of(f->b);
	c_id = id_of(f->c);

	ASSERT_REPLY(run(f, "scratchpad-add", b_id), "OK added 1");
	g_assert_true(is_member(f, f->b));
	ASSERT_REPLY_HAS(run(f, "scratchpad-add", b_id),
	                 "already in the scratchpad");

	/* Embedded: it belongs to its host. */
	f->c->isembedded = TRUE;
	ASSERT_REPLY_HAS(run(f, "scratchpad-add", c_id), "cannot join");
	g_assert_false(f->c->isoverlay);
	f->c->isembedded = FALSE;

	/* Ids that name nothing, or are not ids at all. */
	ASSERT_REPLY_HAS(run(f, "scratchpad-add", "4294967295"),
	                 "no window has id");
	ASSERT_REPLY_HAS(run(f, "scratchpad-add", "-1"), "is not a window id");
	ASSERT_REPLY_HAS(run(f, "scratchpad-add", "7x"), "is not a window id");
	ASSERT_REPLY_HAS(run(f, "scratchpad-add", "99999999999"),
	                 "is not a window id");

	/* A shown dropdown shell is already somebody's overlay. */
	f->c->isoverlay = TRUE;
	f->c->overlay_visible = TRUE;
	g_list_free(f->comp->fstack);
	f->comp->fstack = g_list_append(NULL, f->c);
	ASSERT_REPLY_HAS(run(f, "scratchpad-add", NULL), "cannot join");
	g_assert_cmpuint(f->c->overlay_group, ==, 0);

	/* Nothing focused at all. */
	g_clear_pointer(&f->comp->fstack, g_list_free);
	ASSERT_REPLY(run(f, "scratchpad-add", NULL),
	             "ERROR no window is focused");
}

/* Toggle, show and hide, as the module keeps count of them. */
static void
test_toggle_show_hide(Fixture *f, gconstpointer data)
{
	(void)data;
	NEED_MODULE(f);
	ASSERT_REPLY_HAS(run(f, "scratchpad-toggle", NULL),
	                 "ERROR the scratchpad is empty");
	ASSERT_REPLY_HAS(run(f, "scratchpad-show", NULL),
	                 "ERROR the scratchpad is empty");

	ASSERT_REPLY(run(f, "scratchpad-add", NULL), "OK added 1");
	ASSERT_REPLY(run(f, "scratchpad-toggle", NULL), "OK shown 1");
	ASSERT_REPLY_HAS(status(f), "visible=1 ");
	ASSERT_REPLY(run(f, "scratchpad-toggle", NULL), "OK hidden");
	ASSERT_REPLY_HAS(status(f), "visible=0 ");

	/* show and hide say what they did, however often. */
	ASSERT_REPLY(run(f, "scratchpad-show", NULL), "OK shown 1");
	ASSERT_REPLY(run(f, "scratchpad-show", NULL), "OK shown 1");
	ASSERT_REPLY_HAS(status(f), "visible=1 ");
	ASSERT_REPLY(run(f, "scratchpad-hide", NULL), "OK hidden");
	ASSERT_REPLY(run(f, "scratchpad-hide", NULL), "OK hidden");
	ASSERT_REPLY_HAS(status(f), "visible=0 ");

	/* The typed entry point toggles too; the name is ignored. */
	gowl_scratchpad_handler_toggle_scratchpad(
		GOWL_SCRATCHPAD_HANDLER(f->module), "anything");
	ASSERT_REPLY_HAS(status(f), "visible=1 ");
}

/* Focus going anywhere but the panel rolls it away, as it does the
 * dropdown; focus on one of its own windows keeps it up. */
static void
test_focus_elsewhere_hides(Fixture *f, gconstpointer data)
{
	(void)data;
	NEED_MODULE(f);
	ASSERT_REPLY(run(f, "scratchpad-add", NULL), "OK added 1");
	ASSERT_REPLY(run(f, "scratchpad-show", NULL), "OK shown 1");

	g_signal_emit_by_name(f->comp, "focus-changed", f->a);
	ASSERT_REPLY_HAS(status(f), "visible=1 ");
	g_signal_emit_by_name(f->comp, "focus-changed", f->b);
	ASSERT_REPLY_HAS(status(f), "visible=0 ");

	/* Focus cleared -- a lock, a launcher taking the keyboard -- too. */
	ASSERT_REPLY(run(f, "scratchpad-show", NULL), "OK shown 1");
	g_signal_emit_by_name(f->comp, "focus-changed", NULL);
	ASSERT_REPLY_HAS(status(f), "visible=0 ");
}

/* A window leaves onto the tags in view: tiled if it was tiled, floating
 * where it floated if it floated. */
static void
test_remove_gives_the_window_back(Fixture *f, gconstpointer data)
{
	g_autofree gchar *a_id = NULL;
	g_autofree gchar *b_id = NULL;

	(void)data;
	NEED_MODULE(f);
	a_id = id_of(f->a);
	b_id = id_of(f->b);

	f->b->isfloating = TRUE;
	f->b->geom.x = 100;
	f->b->geom.y = 200;
	f->b->geom.width = 300;
	f->b->geom.height = 400;

	ASSERT_REPLY(run(f, "scratchpad-add", a_id), "OK added 1");
	ASSERT_REPLY(run(f, "scratchpad-add", b_id), "OK added 2");
	/* In the panel it has the panel's geometry, not its own. */
	f->b->geom.x = 960;
	f->b->geom.y = 380;
	f->b->geom.width = 960;
	f->b->geom.height = 700;

	ASSERT_REPLY(run(f, "scratchpad-remove", a_id), "OK removed 1");
	g_assert_false(is_member(f, f->a));
	g_assert_false(f->a->isoverlay);
	g_assert_cmpuint(f->a->overlay_group, ==, 0);
	g_assert_false(f->a->isfloating);
	g_assert_true(f->a->mon == f->mon);
	g_assert_cmpuint(f->a->tags, ==, 1);

	ASSERT_REPLY(run(f, "scratchpad-remove", b_id), "OK removed 0");
	g_assert_false(f->b->isoverlay);
	g_assert_true(f->b->isfloating);
	g_assert_cmpint(f->b->geom.x, ==, 100);
	g_assert_cmpint(f->b->geom.y, ==, 200);
	g_assert_cmpint(f->b->geom.width, ==, 300);
	g_assert_cmpint(f->b->geom.height, ==, 400);

	ASSERT_REPLY_HAS(run(f, "scratchpad-remove", a_id),
	                 "not in the scratchpad");
}

/* The keybind form acts on the focused window, which for a member means
 * the panel is up and that window has the keyboard. */
static void
test_remove_focused(Fixture *f, gconstpointer data)
{
	(void)data;
	NEED_MODULE(f);
	ASSERT_REPLY(run(f, "scratchpad-add", NULL), "OK added 1");
	ASSERT_REPLY(run(f, "scratchpad-show", NULL), "OK shown 1");
	/* What showing it does in a live session. */
	f->a->overlay_visible = TRUE;
	f->comp->fstack = g_list_remove(f->comp->fstack, f->a);
	f->comp->fstack = g_list_prepend(f->comp->fstack, f->a);

	ASSERT_REPLY(run(f, "scratchpad-remove", NULL), "OK removed 0");
	g_assert_false(f->a->isoverlay);
	g_assert_cmpuint(f->a->tags, ==, 1);
	ASSERT_REPLY_HAS(status(f), "visible=0 count=0 ");

	/* No longer a member: refused, and nothing changes. */
	ASSERT_REPLY_HAS(run(f, "scratchpad-remove", NULL),
	                 "not in the scratchpad");
}

/* A member that unmaps, or is destroyed, leaves; the rest stay up. */
static void
test_member_goes_away(Fixture *f, gconstpointer data)
{
	g_autofree gchar *b_id = NULL;

	(void)data;
	NEED_MODULE(f);
	b_id = id_of(f->b);
	ASSERT_REPLY(run(f, "scratchpad-add", NULL), "OK added 1");
	ASSERT_REPLY(run(f, "scratchpad-add", b_id), "OK added 2");
	ASSERT_REPLY(run(f, "scratchpad-show", NULL), "OK shown 2");

	g_signal_emit_by_name(f->comp, "client-removed", f->a);
	g_assert_false(is_member(f, f->a));
	ASSERT_REPLY_HAS(status(f), "visible=1 count=1 ");

	g_signal_emit_by_name(f->b, "destroy");
	g_assert_false(is_member(f, f->b));
	ASSERT_REPLY_HAS(status(f), "visible=0 count=0 members= ");
}

/* Settings parse strictly, and a bad one keeps the value it replaces. */
static void
test_configure(Fixture *f, gconstpointer data)
{
	static const gchar *const good[][2] = {
		{ "enabled",    "true" },
		{ "width-pct",  "0.5" },
		{ "height-pct", " 0.4 " },
		{ "width",      "800" },
		{ "height",     "300" },
		{ "gap",        "8" },
	};
	static const gchar *const bad[][2] = {
		{ "width-pct",  "0" },
		{ "width-pct",  "1.5" },
		{ "width-pct",  "nan" },
		{ "width-pct",  "half" },
		{ "height-pct", "" },
		{ "width",      "-1" },
		{ "width",      "12px" },
		{ "height",     "70000" },
		{ "gap",        "513" },
	};
	const gchar *expect;
	guint i;

	(void)data;
	NEED_MODULE(f);
	expect = "OK visible=0 count=0 members= width-pct=0.5 height-pct=0.4 "
	         "width=800 height=300 gap=8";

	for (i = 0; i < G_N_ELEMENTS(good); i++)
		configure(f, good[i][0], good[i][1]);
	ASSERT_REPLY(status(f), expect);

	for (i = 0; i < G_N_ELEMENTS(bad); i++) {
		g_autofree gchar *needle = g_strdup_printf("%s must be", bad[i][0]);

		expect_warning(needle);
		configure(f, bad[i][0], bad[i][1]);
		if (!saw_expected_warning)
			g_error("%s = \"%s\" was taken without a warning",
			        bad[i][0], bad[i][1]);
		expect_warning(NULL);
	}
	ASSERT_REPLY(status(f), expect);
}

/* Switching the module off gives every window back and stops it
 * listening. */
static void
test_switching_off_gives_windows_back(Fixture *f, gconstpointer data)
{
	g_autofree gchar *b_id = NULL;

	(void)data;
	NEED_MODULE(f);
	b_id = id_of(f->b);
	ASSERT_REPLY(run(f, "scratchpad-add", NULL), "OK added 1");
	ASSERT_REPLY(run(f, "scratchpad-add", b_id), "OK added 2");
	ASSERT_REPLY(run(f, "scratchpad-show", NULL), "OK shown 2");

	gowl_module_deactivate(f->module);
	g_assert_false(f->a->isoverlay);
	g_assert_false(f->b->isoverlay);
	g_assert_cmpuint(f->a->tags, ==, 1);
	g_assert_cmpuint(f->b->tags, ==, 1);
	ASSERT_REPLY_HAS(status(f), "visible=0 count=0 ");

	/* Nothing is listening any more. */
	g_signal_emit_by_name(f->comp, "focus-changed", f->c);
	g_signal_emit_by_name(f->comp, "client-removed", f->a);
	ASSERT_REPLY_HAS(status(f), "visible=0 count=0 ");
}

/* Only scratchpad- words are the module's, and an unknown one of those is
 * an error rather than silence. */
static void
test_commands(Fixture *f, gconstpointer data)
{
	(void)data;
	NEED_MODULE(f);
	g_assert_null(run(f, "expo", NULL));
	g_assert_null(run(f, "scratchpad", NULL));
	ASSERT_REPLY_HAS(run(f, "scratchpad-bogus", NULL),
	                 "ERROR unknown command scratchpad-bogus");
}

/* Before startup there is no compositor to act on, but status answers. */
static void
test_before_startup(void)
{
	GowlModule *module;
	gchar      *reply;
	GType       type;

	type = scratchpad_type();
	if (type == 0) {
		g_test_skip("scratchpad.so did not load");
		return;
	}
	module = (GowlModule *)g_object_new(type, NULL);
	reply = gowl_ipc_handler_handle_command(GOWL_IPC_HANDLER(module),
	                                        "scratchpad-toggle", NULL);
	g_assert_cmpstr(reply, ==, "ERROR the scratchpad has not started");
	g_free(reply);
	reply = gowl_ipc_handler_handle_command(GOWL_IPC_HANDLER(module),
	                                        "scratchpad-status", NULL);
	g_assert_cmpstr(reply, ==, "OK visible=0 count=0 members= width-pct=1 "
	                "height-pct=0.666667 width=0 height=0 gap=0");
	g_free(reply);
	g_object_unref(module);
}

#define ADD(path, fn) \
	g_test_add("/scratchpad-module/" path, Fixture, NULL, \
	           fixture_setup, fn, fixture_teardown)

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	ADD("add/focused", test_add_focused);
	ADD("add/refusals", test_add_refusals);
	ADD("toggle-show-hide", test_toggle_show_hide);
	ADD("focus-elsewhere-hides", test_focus_elsewhere_hides);
	ADD("remove/gives-the-window-back", test_remove_gives_the_window_back);
	ADD("remove/focused", test_remove_focused);
	ADD("member-goes-away", test_member_goes_away);
	ADD("configure", test_configure);
	ADD("switching-off-gives-windows-back",
	    test_switching_off_gives_windows_back);
	ADD("commands", test_commands);
	g_test_add_func("/scratchpad-module/before-startup", test_before_startup);

	return g_test_run();
}
