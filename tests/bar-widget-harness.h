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
 * A harness for testing ONE shipped bar widget without a compositor.
 *
 * The widgets are builtins of bar.so, which exports both the call that
 * registers each group and every non-static helper, so a test loads it
 * with GModule, registers the group into a private registry,
 * instantiates the widget, and hands it a stub host that:
 *
 *   - runs queued work INLINE (or holds it, with `hold_work'), so a
 *     worker's whole effect is visible when queue_work returns;
 *   - records every spawn, toast and panel-refresh request;
 *   - gives the widget a private state directory.
 *
 * Header-only on purpose: tests are one .c each (Makefile wildcard), and
 * this is shared by all the widget tests.  Every function is static and
 * marked unused-ok, so including it costs a test nothing it does not
 * call.
 *
 * Usage:
 *
 *   #define BAR_HARNESS_MODULE GOWL_TEST_BAR_MODULE
 *   #include "bar-widget-harness.h"
 *
 *   BarHarness h;
 *   if (!bar_harness_up(&h, "bar_register_desktop_plugins", "wow-gold"))
 *       return;   (skipped: bar.so not built)
 *   bar_harness_set(&h, "host", "");
 *   ...
 *   bar_harness_down(&h);
 */

#ifndef BAR_WIDGET_HARNESS_H
#define BAR_WIDGET_HARNESS_H

#include <glib.h>
#include <glib/gstdio.h>
#include <gmodule.h>
#include <string.h>
#include <stdlib.h>

#include "barkit/gowl-bar-plugin.h"
#include "barkit/gowl-bar-registry.h"
#include "barkit/gowl-bar-panel.h"
#include "barkit/gowl-bar-theme.h"
#include "barkit/gowl-bar-toast.h"
#include "barkit/gowl-bar-host.h"
#include "barkit/gowl-bar-panel-render.h"

#include <pango/pangocairo.h>

#ifndef BAR_HARNESS_MODULE
#define BAR_HARNESS_MODULE "build/release/modules/bar.so"
#endif

/* ── The stub host ────────────────────────────────────────────────── */

typedef struct {
	GowlBarPlugin   *plugin;
	GowlBarWorkFunc  func;
	gpointer         user_data;
	GDestroyNotify   destroy;
} BarHarnessWork;

typedef struct {
	GObject       parent;
	GowlBarTheme *theme;
	gchar        *state_dir;
	guint         redraws;
	guint         refreshes;
	guint         opened;
	guint         closed;
	gboolean      hold_work;     /* queue instead of running inline */
	GPtrArray    *held;          /* BarHarnessWork*, when holding */
	GPtrArray    *spawned;       /* gchar* command lines */
	GPtrArray    *toasts;        /* gchar* "summary|body" */
	GHashTable   *bar_settings;  /* set_bar_setting calls */
} BarStubHost;

typedef struct { GObjectClass parent; } BarStubHostClass;

static void bar_stub_host_iface_init(GowlBarHostInterface *iface);

G_DEFINE_TYPE_WITH_CODE(BarStubHost, bar_stub_host, G_TYPE_OBJECT,
	G_IMPLEMENT_INTERFACE(GOWL_TYPE_BAR_HOST, bar_stub_host_iface_init))

static const GowlBarTheme *
bar_stub_get_theme(GowlBarHost *host)
{
	return ((BarStubHost *)host)->theme;
}

static void
bar_stub_request_redraw(GowlBarHost *host)
{
	((BarStubHost *)host)->redraws++;
}

static void
bar_stub_request_panel_refresh(GowlBarHost *host, GowlBarPlugin *plugin)
{
	(void)plugin;
	g_atomic_int_inc((gint *)&((BarStubHost *)host)->refreshes);
}

static void
bar_stub_open_panel(GowlBarHost *host, const gchar *plugin_id)
{
	(void)plugin_id;
	((BarStubHost *)host)->opened++;
}

static void
bar_stub_close_panel(GowlBarHost *host)
{
	((BarStubHost *)host)->closed++;
}

static void
bar_stub_notify(GowlBarHost *host, GowlBarToast *toast)
{
	BarStubHost *self = (BarStubHost *)host;

	/* `summary|body', then `|cmd:COMMAND' when a click runs one. */
	g_ptr_array_add(self->toasts, g_strdup_printf("%s|%s%s%s",
		gowl_bar_toast_get_summary(toast),
		gowl_bar_toast_get_body(toast) != NULL
			? gowl_bar_toast_get_body(toast) : "",
		gowl_bar_toast_get_command(toast) != NULL ? "|cmd:" : "",
		gowl_bar_toast_get_command(toast) != NULL
			? gowl_bar_toast_get_command(toast) : ""));
	/* The host owns the toast it is handed. */
	gowl_bar_toast_free(toast);
}

static void
bar_stub_queue_work(GowlBarHost *host, GowlBarPlugin *plugin,
                    GowlBarWorkFunc func, gpointer user_data,
                    GDestroyNotify destroy)
{
	BarStubHost *self = (BarStubHost *)host;

	if (self->hold_work) {
		BarHarnessWork *w = g_new0(BarHarnessWork, 1);

		w->plugin    = g_object_ref(plugin);
		w->func      = func;
		w->user_data = user_data;
		w->destroy   = destroy;
		g_ptr_array_add(self->held, w);
		return;
	}
	func(plugin, user_data);
	if (destroy != NULL && user_data != NULL)
		destroy(user_data);
}

static void
bar_stub_spawn(GowlBarHost *host, const gchar *cmdline)
{
	g_ptr_array_add(((BarStubHost *)host)->spawned, g_strdup(cmdline));
}

static const gchar *
bar_stub_get_state_dir(GowlBarHost *host)
{
	return ((BarStubHost *)host)->state_dir;
}

static gboolean
bar_stub_set_bar_setting(GowlBarHost *host, const gchar *key,
                         const gchar *value)
{
	g_hash_table_insert(((BarStubHost *)host)->bar_settings,
	                    g_strdup(key), g_strdup(value));
	return TRUE;
}

static void
bar_stub_host_iface_init(GowlBarHostInterface *iface)
{
	iface->get_theme             = bar_stub_get_theme;
	iface->request_redraw        = bar_stub_request_redraw;
	iface->request_panel_refresh = bar_stub_request_panel_refresh;
	iface->open_panel            = bar_stub_open_panel;
	iface->close_panel           = bar_stub_close_panel;
	iface->notify                = bar_stub_notify;
	iface->queue_work            = bar_stub_queue_work;
	iface->spawn                 = bar_stub_spawn;
	iface->get_state_dir         = bar_stub_get_state_dir;
	iface->set_bar_setting       = bar_stub_set_bar_setting;
}

static void
bar_stub_host_finalize(GObject *object)
{
	BarStubHost *self = (BarStubHost *)object;

	gowl_bar_theme_free(self->theme);
	g_free(self->state_dir);
	g_ptr_array_unref(self->held);
	g_ptr_array_unref(self->spawned);
	g_ptr_array_unref(self->toasts);
	g_hash_table_unref(self->bar_settings);
	G_OBJECT_CLASS(bar_stub_host_parent_class)->finalize(object);
}

static void
bar_stub_host_class_init(BarStubHostClass *klass)
{
	G_OBJECT_CLASS(klass)->finalize = bar_stub_host_finalize;
}

static void
bar_stub_host_init(BarStubHost *self)
{
	self->theme   = gowl_bar_theme_new();
	self->held    = g_ptr_array_new();
	self->spawned = g_ptr_array_new_with_free_func(g_free);
	self->toasts  = g_ptr_array_new_with_free_func(g_free);
	self->bar_settings = g_hash_table_new_full(g_str_hash, g_str_equal,
	                                           g_free, g_free);
}

/* ── The harness ──────────────────────────────────────────────────── */

typedef struct {
	GModule         *bar_so;
	GowlBarRegistry *registry;
	GowlBarPlugin   *plugin;
	BarStubHost     *host;
	gchar           *tmp;        /* a private scratch dir, removed */
} BarHarness;

/*
 * Look a symbol up in bar.so.  Returns NULL (and the caller should
 * skip) when it is not there.
 */
G_GNUC_UNUSED static gpointer
bar_harness_sym(BarHarness *h, const gchar *name)
{
	gpointer sym = NULL;

	if (h->bar_so == NULL ||
	    !g_module_symbol(h->bar_so, name, &sym))
		return NULL;
	return sym;
}

/*
 * Load bar.so, register @group (e.g. "bar_register_desktop_plugins")
 * into a private registry and instantiate @widget with a stub host.
 * @widget may be NULL to only load the module (for helper tests).
 *
 * Returns FALSE, having marked the test skipped, when bar.so is not
 * built or does not carry the group.
 */
G_GNUC_UNUSED static gboolean
bar_harness_up(BarHarness *h, const gchar *group, const gchar *widget)
{
	void (*reg)(GowlBarRegistry *) = NULL;
	GError *error = NULL;
	const gchar *parent;

	memset(h, 0, sizeof(*h));

	h->bar_so = g_module_open(BAR_HARNESS_MODULE, G_MODULE_BIND_LAZY);
	if (h->bar_so == NULL) {
		g_test_skip(g_module_error());
		return FALSE;
	}
	g_module_make_resident(h->bar_so);

	parent = g_get_tmp_dir();
	h->tmp = g_build_filename(parent, "gowl-bar-widget-XXXXXX", NULL);
	g_assert_nonnull(g_mkdtemp_full(h->tmp, 0700));

	h->host = g_object_new(bar_stub_host_get_type(), NULL);
	h->host->state_dir = g_build_filename(h->tmp, "state", NULL);
	g_mkdir_with_parents(h->host->state_dir, 0700);

	if (widget == NULL)
		return TRUE;

	reg = (void (*)(GowlBarRegistry *))bar_harness_sym(h, group);
	if (reg == NULL) {
		g_test_skip("bar.so does not export the registration call");
		return FALSE;
	}
	h->registry = gowl_bar_registry_new(NULL);
	reg(h->registry);
	h->plugin = gowl_bar_registry_instantiate(h->registry, widget,
	                                          &error);
	g_assert_no_error(error);
	g_assert_nonnull(h->plugin);
	gowl_bar_plugin_set_host(h->plugin, GOWL_BAR_HOST(h->host));
	return TRUE;
}

/* Activate the widget after its settings are in place. */
G_GNUC_UNUSED static void
bar_harness_activate(BarHarness *h)
{
	GError *error = NULL;

	g_assert_true(gowl_bar_plugin_activate(h->plugin, &error));
	g_assert_no_error(error);
}

/* Set one widget setting and run the configure hook, as a config push
   (or cmacs's gowl-bar-configure) does. */
G_GNUC_UNUSED static void
bar_harness_set(BarHarness *h, const gchar *key, const gchar *value)
{
	gowl_bar_plugin_set_setting(h->plugin, key, value);
	gowl_bar_plugin_configure(h->plugin, NULL);
}

/* Run every held work item, in order, until none are left. */
G_GNUC_UNUSED static guint
bar_harness_run_held(BarHarness *h)
{
	guint ran = 0;

	while (h->host->held->len > 0) {
		BarHarnessWork *w = g_ptr_array_steal_index(h->host->held, 0);

		w->func(w->plugin, w->user_data);
		if (w->destroy != NULL && w->user_data != NULL)
			w->destroy(w->user_data);
		g_object_unref(w->plugin);
		g_free(w);
		ran++;
	}
	return ran;
}

/* One poll, async half included (inline). */
G_GNUC_UNUSED static void
bar_harness_poll(BarHarness *h)
{
	gowl_bar_plugin_poll(h->plugin);
	if (gowl_bar_plugin_wants_async(h->plugin))
		gowl_bar_plugin_poll_async(h->plugin);
}

/* The panel, freshly built.  Caller unrefs. */
G_GNUC_UNUSED static GowlBarPanel *
bar_harness_panel(BarHarness *h)
{
	return gowl_bar_plugin_build_panel(h->plugin);
}

/* Does the panel contain an item whose title, value, subtitle or
   title2/value2 contains @needle? */
G_GNUC_UNUSED static gboolean
bar_harness_panel_mentions(GowlBarPanel *panel, const gchar *needle)
{
	guint i, n;

	n = gowl_bar_panel_n_items(panel);
	for (i = 0; i < n; i++) {
		GowlBarPanelItem *item = gowl_bar_panel_get_item(panel, i);
		const gchar *fields[6];
		guint f, c;

		fields[0] = gowl_bar_panel_item_get_title(item);
		fields[1] = gowl_bar_panel_item_get_value(item);
		fields[2] = gowl_bar_panel_item_get_subtitle(item);
		fields[3] = gowl_bar_panel_item_get_title2(item);
		fields[4] = gowl_bar_panel_item_get_value2(item);
		fields[5] = gowl_bar_panel_item_get_id(item);
		for (f = 0; f < G_N_ELEMENTS(fields); f++)
			if (fields[f] != NULL && strstr(fields[f], needle))
				return TRUE;
		for (c = 0; c < gowl_bar_panel_item_n_children(item); c++) {
			GowlBarPanelItem *child;

			child = gowl_bar_panel_item_get_child(item, c);
			if (gowl_bar_panel_item_get_title(child) != NULL &&
			    strstr(gowl_bar_panel_item_get_title(child),
			           needle))
				return TRUE;
		}
	}
	return FALSE;
}

/* Paint a canvas through the plugin, as the bar does. */
static void
bar_harness_canvas_draw(gpointer user_data, gint item_index,
                        GowlBarPanelItem *item, cairo_t *cr, gint width,
                        gint height)
{
	BarHarness *h = user_data;

	(void)item_index;
	gowl_bar_plugin_panel_canvas(h->plugin, gowl_bar_panel_item_get_id(item),
	                             cr, h->host->theme, width, height);
}

/*
 * When GOWL_TEST_DUMP_PANELS names a directory, render @panel there as
 * @name.png, whole (no height cap), framed as the bar frames it.  For a
 * human looking at a layout; no test asserts on the picture.  Returns
 * the content height, or 0 when nothing was written.
 */
G_GNUC_UNUSED static gint
bar_harness_dump_panel(BarHarness *h, GowlBarPanel *panel, const gchar *name)
{
	const gchar *dir = g_getenv("GOWL_TEST_DUMP_PANELS");
	const GowlBarTheme *theme = h->host->theme;
	g_autofree gchar *file = NULL;
	g_autofree gchar *path = NULL;
	GowlBarPanelRenderCtx ctx;
	PangoFontDescription *font;
	cairo_surface_t *cs;
	PangoLayout *layout;
	cairo_t *cr;
	gint width, height;

	if (dir == NULL || dir[0] == '\0' || panel == NULL)
		return 0;
	width = gowl_bar_panel_get_width(panel);
	if (width <= 0)
		width = gowl_bar_theme_metric(theme,
		                              GOWL_BAR_METRIC_PANEL_WIDTH);

	/* Measure on a scratch surface, then draw at the full height. */
	cs = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
	cr = cairo_create(cs);
	layout = pango_cairo_create_layout(cr);
	font = pango_font_description_from_string(
		gowl_bar_theme_get_font(theme));
	pango_layout_set_font_description(layout, font);
	height = gowl_bar_panel_measure(panel, layout, theme, width);
	g_object_unref(layout);
	cairo_destroy(cr);
	cairo_surface_destroy(cs);

	cs = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width + 48,
	                                height + 48);
	cr = cairo_create(cs);
	gowl_bar_panel_draw_frame(cr, theme, 24, 24, width, height, NULL);
	cairo_translate(cr, 24, 24);
	layout = pango_cairo_create_layout(cr);
	pango_layout_set_font_description(layout, font);
	gowl_bar_panel_render_ctx_init(&ctx, width);
	ctx.canvas_draw = bar_harness_canvas_draw;
	ctx.canvas_data = h;
	gowl_bar_panel_render(panel, cr, layout, theme, &ctx);
	g_object_unref(layout);
	pango_font_description_free(font);

	file = g_strconcat(name, ".png", NULL);
	path = g_build_filename(dir, file, NULL);
	g_mkdir_with_parents(dir, 0700);
	if (cairo_surface_write_to_png(cs, path) != CAIRO_STATUS_SUCCESS)
		g_test_message("could not write %s", path);
	cairo_destroy(cr);
	cairo_surface_destroy(cs);
	return height;
}

/* Write an executable script into the scratch dir; returns its path. */
G_GNUC_UNUSED static gchar *
bar_harness_script(BarHarness *h, const gchar *name, const gchar *body)
{
	gchar *path;
	g_autofree gchar *text = NULL;

	path = g_build_filename(h->tmp, name, NULL);
	text = g_strdup_printf("#!/bin/bash\n%s\n", body);
	g_assert_true(g_file_set_contents(path, text, -1, NULL));
	g_assert_cmpint(g_chmod(path, 0700), ==, 0);
	return path;
}

/* A path inside the scratch dir. */
G_GNUC_UNUSED static gchar *
bar_harness_path(BarHarness *h, const gchar *name)
{
	return g_build_filename(h->tmp, name, NULL);
}

/* Did any spawn's command line contain @needle? */
G_GNUC_UNUSED static gboolean
bar_harness_spawned(BarHarness *h, const gchar *needle)
{
	guint i;

	for (i = 0; i < h->host->spawned->len; i++)
		if (strstr(g_ptr_array_index(h->host->spawned, i), needle))
			return TRUE;
	return FALSE;
}

/* Forget the toasts raised so far. */
G_GNUC_UNUSED static void
bar_harness_toasts_clear(BarHarness *h)
{
	g_ptr_array_set_size(h->host->toasts, 0);
}

/* Did any toast contain @needle? */
G_GNUC_UNUSED static gboolean
bar_harness_toasted(BarHarness *h, const gchar *needle)
{
	guint i;

	for (i = 0; i < h->host->toasts->len; i++)
		if (strstr(g_ptr_array_index(h->host->toasts, i), needle))
			return TRUE;
	return FALSE;
}

G_GNUC_UNUSED static void
bar_harness_down(BarHarness *h)
{
	bar_harness_run_held(h);
	if (h->plugin != NULL) {
		gowl_bar_plugin_deactivate(h->plugin);
		gowl_bar_plugin_set_host(h->plugin, NULL);
		g_clear_object(&h->plugin);
	}
	g_clear_object(&h->registry);
	g_clear_object(&h->host);
	if (h->tmp != NULL) {
		g_autofree gchar *cmd = g_strdup_printf("rm -rf '%s'", h->tmp);

		if (system(cmd) != 0)
			g_test_message("could not remove %s", h->tmp);
	}
	g_clear_pointer(&h->tmp, g_free);
	/* bar.so stays resident: GType registrations cannot be undone. */
}

#endif /* BAR_WIDGET_HARNESS_H */
