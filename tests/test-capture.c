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
 * Tests for the screencast capture abstraction: the GowlCaptureSource
 * boxed type, the wlroots version-compat macros, and the
 * GowlCaptureProvider interface (capability reporting, dispatch, signals,
 * and source-list edge cases) exercised via an in-test mock provider so
 * the tests run identically regardless of which wlroots is compiled in.
 */

#include <glib-object.h>

#include "gowl-wlroots-compat.h"
#include "boxed/gowl-capture-source.h"
#include "interfaces/gowl-capture-provider.h"

/* ------------------------------------------------------------------ *
 * GowlCaptureSource boxed type
 * ------------------------------------------------------------------ */

static void
test_source_new(void)
{
	GowlCaptureSource *s;

	s = gowl_capture_source_new(GOWL_CAPTURE_SOURCE_WINDOW,
	                            "win-1", "Editor", "org.gnome.gedit");
	g_assert_nonnull(s);
	g_assert_cmpint(s->kind, ==, GOWL_CAPTURE_SOURCE_WINDOW);
	g_assert_cmpstr(s->id, ==, "win-1");
	g_assert_cmpstr(s->title, ==, "Editor");
	g_assert_cmpstr(s->app_id, ==, "org.gnome.gedit");
	gowl_capture_source_free(s);
}

static void
test_source_monitor(void)
{
	GowlCaptureSource *s;

	s = gowl_capture_source_new(GOWL_CAPTURE_SOURCE_MONITOR,
	                            "DP-1", "Dell U2720Q", NULL);
	g_assert_cmpint(s->kind, ==, GOWL_CAPTURE_SOURCE_MONITOR);
	g_assert_cmpstr(s->id, ==, "DP-1");
	g_assert_null(s->app_id);
	gowl_capture_source_free(s);
}

static void
test_source_copy(void)
{
	GowlCaptureSource *s;
	GowlCaptureSource *c;

	s = gowl_capture_source_new(GOWL_CAPTURE_SOURCE_WINDOW,
	                            "id", "title", "app");
	c = gowl_capture_source_copy(s);
	g_assert_nonnull(c);
	g_assert_true(c != s);
	/* Deep copy: strings duplicated, not aliased. */
	g_assert_true(c->id != s->id);
	g_assert_true(gowl_capture_source_equals(s, c));
	gowl_capture_source_free(s);
	gowl_capture_source_free(c);
}

static void
test_source_equals(void)
{
	GowlCaptureSource *a, *b, *c;

	a = gowl_capture_source_new(GOWL_CAPTURE_SOURCE_WINDOW, "1", "t", "a");
	b = gowl_capture_source_new(GOWL_CAPTURE_SOURCE_WINDOW, "1", "t", "a");
	c = gowl_capture_source_new(GOWL_CAPTURE_SOURCE_WINDOW, "2", "t", "a");

	g_assert_true(gowl_capture_source_equals(a, b));
	g_assert_false(gowl_capture_source_equals(a, c));
	gowl_capture_source_free(a);
	gowl_capture_source_free(b);
	gowl_capture_source_free(c);
}

static void
test_source_equals_kind_differs(void)
{
	GowlCaptureSource *a, *b;

	/* Same id/title but different kind -> not equal. */
	a = gowl_capture_source_new(GOWL_CAPTURE_SOURCE_MONITOR, "x", "t", NULL);
	b = gowl_capture_source_new(GOWL_CAPTURE_SOURCE_WINDOW, "x", "t", NULL);
	g_assert_false(gowl_capture_source_equals(a, b));
	gowl_capture_source_free(a);
	gowl_capture_source_free(b);
}

static void
test_source_null_strings(void)
{
	GowlCaptureSource *a, *b;

	/* NULL fields are preserved and compare equal to each other. */
	a = gowl_capture_source_new(GOWL_CAPTURE_SOURCE_MONITOR, NULL, NULL, NULL);
	b = gowl_capture_source_new(GOWL_CAPTURE_SOURCE_MONITOR, NULL, NULL, NULL);
	g_assert_null(a->id);
	g_assert_null(a->title);
	g_assert_null(a->app_id);
	g_assert_true(gowl_capture_source_equals(a, b));
	gowl_capture_source_free(a);
	gowl_capture_source_free(b);
}

static void
test_source_null_vs_empty(void)
{
	GowlCaptureSource *a, *b;

	/* "" and NULL are distinct (callers can tell unset from empty). */
	a = gowl_capture_source_new(GOWL_CAPTURE_SOURCE_WINDOW, "i", "", NULL);
	b = gowl_capture_source_new(GOWL_CAPTURE_SOURCE_WINDOW, "i", NULL, NULL);
	g_assert_nonnull(a->title);
	g_assert_null(b->title);
	g_assert_false(gowl_capture_source_equals(a, b));
	gowl_capture_source_free(a);
	gowl_capture_source_free(b);
}

static void
test_source_free_null(void)
{
	/* Must be a safe no-op. */
	gowl_capture_source_free(NULL);
}

static void
test_source_copy_with_nulls(void)
{
	GowlCaptureSource *s, *c;

	s = gowl_capture_source_new(GOWL_CAPTURE_SOURCE_WINDOW, "id", NULL, NULL);
	c = gowl_capture_source_copy(s);
	g_assert_true(gowl_capture_source_equals(s, c));
	g_assert_null(c->title);
	gowl_capture_source_free(s);
	gowl_capture_source_free(c);
}

static void
test_source_gtype(void)
{
	GType t = GOWL_TYPE_CAPTURE_SOURCE;

	g_assert_true(t != G_TYPE_INVALID);
	g_assert_true(G_TYPE_IS_BOXED(t));
}

static void
test_source_boxed_roundtrip(void)
{
	GowlCaptureSource *s, *c;

	/* Exercise the GType copy/free hooks (g_boxed_copy/free) used by
	 * GValue/signal marshalling. */
	s = gowl_capture_source_new(GOWL_CAPTURE_SOURCE_WINDOW, "i", "t", "a");
	c = g_boxed_copy(GOWL_TYPE_CAPTURE_SOURCE, s);
	g_assert_nonnull(c);
	g_assert_true(gowl_capture_source_equals(s, c));
	g_boxed_free(GOWL_TYPE_CAPTURE_SOURCE, c);
	gowl_capture_source_free(s);
}

/* ------------------------------------------------------------------ *
 * Version-compat macros
 * ------------------------------------------------------------------ */

static void
test_compat_version_consistency(void)
{
	/* The encoded version must agree with the major/minor inputs. */
	g_assert_cmpint(GOWL_WLROOTS_VERSION, ==,
	                GOWL_WLROOTS_VERSION_MAJOR * 100
	                + GOWL_WLROOTS_VERSION_MINOR);

	/* HAVE_0_20 and CAPTURE_WINDOW must track each other (today). */
	g_assert_cmpint(GOWL_HAVE_WLROOTS_0_20, ==, GOWL_HAVE_CAPTURE_WINDOW);

	/* The 0.20 flag must match an actual >= 0.20 version number. */
	if (GOWL_WLROOTS_VERSION >= 20)
		g_assert_cmpint(GOWL_HAVE_WLROOTS_0_20, ==, 1);
	else
		g_assert_cmpint(GOWL_HAVE_WLROOTS_0_20, ==, 0);
}

/* ------------------------------------------------------------------ *
 * Mock GowlCaptureProvider -- exercises interface dispatch + signals
 * ------------------------------------------------------------------ */

#define TEST_TYPE_PROVIDER (test_provider_get_type())
G_DECLARE_FINAL_TYPE(TestProvider, test_provider, TEST, PROVIDER, GObject)

struct _TestProvider {
	GObject parent_instance;
	GowlCaptureCapability caps;
	GList *windows;  /* list of dummy gpointer client ids */
};

static void test_provider_iface_init(GowlCaptureProviderInterface *iface);

G_DEFINE_TYPE_WITH_CODE(TestProvider, test_provider, G_TYPE_OBJECT,
	G_IMPLEMENT_INTERFACE(GOWL_TYPE_CAPTURE_PROVIDER,
	                      test_provider_iface_init))

static GowlCaptureCapability
test_provider_caps(GowlCaptureProvider *p)
{
	return ((TestProvider *)p)->caps;
}

static GList *
test_provider_list(GowlCaptureProvider *p)
{
	TestProvider *self = (TestProvider *)p;
	GList *out = NULL, *l;

	/* One monitor always, plus one window per tracked client. */
	out = g_list_prepend(out,
		gowl_capture_source_new(GOWL_CAPTURE_SOURCE_MONITOR,
		                        "MON", "MON", NULL));
	for (l = self->windows; l != NULL; l = l->next) {
		gchar *id = g_strdup_printf("win-%p", l->data);
		out = g_list_prepend(out,
			gowl_capture_source_new(GOWL_CAPTURE_SOURCE_WINDOW,
			                        id, "w", "a"));
		g_free(id);
	}
	return g_list_reverse(out);
}

static void
test_provider_add(GowlCaptureProvider *p, gpointer client)
{
	TestProvider *self = (TestProvider *)p;
	GowlCaptureSource *s;

	self->windows = g_list_append(self->windows, client);
	s = gowl_capture_source_new(GOWL_CAPTURE_SOURCE_WINDOW, "id", "w", "a");
	g_signal_emit_by_name(p, "source-added", s);
	gowl_capture_source_free(s);
}

static void
test_provider_remove(GowlCaptureProvider *p, gpointer client)
{
	TestProvider *self = (TestProvider *)p;
	GowlCaptureSource *s;

	self->windows = g_list_remove(self->windows, client);
	s = gowl_capture_source_new(GOWL_CAPTURE_SOURCE_WINDOW, "id", "w", "a");
	g_signal_emit_by_name(p, "source-removed", s);
	gowl_capture_source_free(s);
}

static void
test_provider_iface_init(GowlCaptureProviderInterface *iface)
{
	iface->get_capabilities = test_provider_caps;
	iface->list_sources     = test_provider_list;
	iface->add_window       = test_provider_add;
	iface->remove_window    = test_provider_remove;
	/* create_globals / update_window intentionally left NULL to test
	 * the dispatch wrappers' NULL-method handling. */
}

static void
test_provider_finalize(GObject *o)
{
	g_list_free(((TestProvider *)o)->windows);
	G_OBJECT_CLASS(test_provider_parent_class)->finalize(o);
}

static void
test_provider_class_init(TestProviderClass *k)
{
	G_OBJECT_CLASS(k)->finalize = test_provider_finalize;
}

static void
test_provider_init(TestProvider *self)
{
	self->caps = GOWL_CAPTURE_CAP_MONITOR;
	self->windows = NULL;
}

static GowlCaptureProvider *
make_provider(GowlCaptureCapability caps)
{
	TestProvider *p = g_object_new(TEST_TYPE_PROVIDER, NULL);
	p->caps = caps;
	return GOWL_CAPTURE_PROVIDER(p);
}

/* ---- interface tests ---- */

static void
test_iface_capabilities_monitor_only(void)
{
	GowlCaptureProvider *p = make_provider(GOWL_CAPTURE_CAP_MONITOR);

	g_assert_cmpint(gowl_capture_provider_get_capabilities(p), ==,
	                GOWL_CAPTURE_CAP_MONITOR);
	g_assert_false(gowl_capture_provider_supports_window_capture(p));
	g_object_unref(p);
}

static void
test_iface_capabilities_with_window(void)
{
	GowlCaptureProvider *p = make_provider(
		GOWL_CAPTURE_CAP_MONITOR | GOWL_CAPTURE_CAP_WINDOW);

	g_assert_true(gowl_capture_provider_supports_window_capture(p));
	g_object_unref(p);
}

static void
test_iface_null_method_is_safe(void)
{
	GowlCaptureProvider *p = make_provider(GOWL_CAPTURE_CAP_MONITOR);

	/* create_globals + update_window are NULL on the mock: the
	 * wrappers must not crash and create_globals returns FALSE. */
	g_assert_false(gowl_capture_provider_create_globals(p, NULL));
	gowl_capture_provider_update_window(p, NULL);  /* no-op */
	g_object_unref(p);
}

static void
test_iface_list_sources_monitor_only(void)
{
	GowlCaptureProvider *p = make_provider(GOWL_CAPTURE_CAP_MONITOR);
	GList *sources = gowl_capture_provider_list_sources(p);

	/* No windows tracked -> exactly the one monitor. */
	g_assert_cmpuint(g_list_length(sources), ==, 1);
	g_assert_cmpint(((GowlCaptureSource *)sources->data)->kind, ==,
	                GOWL_CAPTURE_SOURCE_MONITOR);
	g_list_free_full(sources,
	                 (GDestroyNotify)gowl_capture_source_free);
	g_object_unref(p);
}

static void
test_iface_add_remove_windows(void)
{
	GowlCaptureProvider *p = make_provider(
		GOWL_CAPTURE_CAP_MONITOR | GOWL_CAPTURE_CAP_WINDOW);
	GList *sources;
	gpointer c1 = GINT_TO_POINTER(1);
	gpointer c2 = GINT_TO_POINTER(2);

	gowl_capture_provider_add_window(p, c1);
	gowl_capture_provider_add_window(p, c2);
	sources = gowl_capture_provider_list_sources(p);
	/* 1 monitor + 2 windows. */
	g_assert_cmpuint(g_list_length(sources), ==, 3);
	g_list_free_full(sources, (GDestroyNotify)gowl_capture_source_free);

	gowl_capture_provider_remove_window(p, c1);
	sources = gowl_capture_provider_list_sources(p);
	/* 1 monitor + 1 window. */
	g_assert_cmpuint(g_list_length(sources), ==, 2);
	g_list_free_full(sources, (GDestroyNotify)gowl_capture_source_free);

	g_object_unref(p);
}

typedef struct {
	guint added;
	guint removed;
} SignalCounter;

static void
on_added(GowlCaptureProvider *p, GowlCaptureSource *s, gpointer data)
{
	(void)p;
	g_assert_nonnull(s);
	((SignalCounter *)data)->added++;
}

static void
on_removed(GowlCaptureProvider *p, GowlCaptureSource *s, gpointer data)
{
	(void)p;
	g_assert_nonnull(s);
	((SignalCounter *)data)->removed++;
}

static void
test_iface_signals(void)
{
	GowlCaptureProvider *p = make_provider(
		GOWL_CAPTURE_CAP_MONITOR | GOWL_CAPTURE_CAP_WINDOW);
	SignalCounter counter = { 0, 0 };
	gpointer c1 = GINT_TO_POINTER(42);

	g_signal_connect(p, "source-added", G_CALLBACK(on_added), &counter);
	g_signal_connect(p, "source-removed", G_CALLBACK(on_removed), &counter);

	gowl_capture_provider_add_window(p, c1);
	gowl_capture_provider_remove_window(p, c1);

	g_assert_cmpuint(counter.added, ==, 1);
	g_assert_cmpuint(counter.removed, ==, 1);
	g_object_unref(p);
}

static void
test_iface_gtype_is_interface(void)
{
	g_assert_true(G_TYPE_IS_INTERFACE(GOWL_TYPE_CAPTURE_PROVIDER));
}

int
main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);

	/* Boxed type */
	g_test_add_func("/capture/source/new", test_source_new);
	g_test_add_func("/capture/source/monitor", test_source_monitor);
	g_test_add_func("/capture/source/copy", test_source_copy);
	g_test_add_func("/capture/source/equals", test_source_equals);
	g_test_add_func("/capture/source/equals-kind",
	                test_source_equals_kind_differs);
	g_test_add_func("/capture/source/null-strings",
	                test_source_null_strings);
	g_test_add_func("/capture/source/null-vs-empty",
	                test_source_null_vs_empty);
	g_test_add_func("/capture/source/free-null", test_source_free_null);
	g_test_add_func("/capture/source/copy-with-nulls",
	                test_source_copy_with_nulls);
	g_test_add_func("/capture/source/gtype", test_source_gtype);
	g_test_add_func("/capture/source/boxed-roundtrip",
	                test_source_boxed_roundtrip);

	/* Version compat */
	g_test_add_func("/capture/compat/version-consistency",
	                test_compat_version_consistency);

	/* Interface */
	g_test_add_func("/capture/iface/caps-monitor-only",
	                test_iface_capabilities_monitor_only);
	g_test_add_func("/capture/iface/caps-with-window",
	                test_iface_capabilities_with_window);
	g_test_add_func("/capture/iface/null-method-safe",
	                test_iface_null_method_is_safe);
	g_test_add_func("/capture/iface/list-monitor-only",
	                test_iface_list_sources_monitor_only);
	g_test_add_func("/capture/iface/add-remove",
	                test_iface_add_remove_windows);
	g_test_add_func("/capture/iface/signals", test_iface_signals);
	g_test_add_func("/capture/iface/gtype", test_iface_gtype_is_interface);

	return g_test_run();
}
