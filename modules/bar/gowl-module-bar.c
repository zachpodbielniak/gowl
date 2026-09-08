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
#define G_LOG_DOMAIN "gowl-bar"

#define _GNU_SOURCE

#include <glib-object.h>
#include <gmodule.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <linux/input-event-codes.h>

#include <cairo.h>
#include <pango/pangocairo.h>

#include <drm_fourcc.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/types/wlr_scene.h>

#include "gowl-enums.h"
#include "module/gowl-module.h"
#include "interfaces/gowl-bar-provider.h"
#include "interfaces/gowl-ipc-handler.h"
#include "interfaces/gowl-keybind-handler.h"
#include "interfaces/gowl-startup-handler.h"
#include "interfaces/gowl-shutdown-handler.h"
#include "core/gowl-compositor.h"
#include "core/gowl-client.h"
#include "core/gowl-monitor.h"
#include "core/gowl-bar.h"
#include "config/gowl-config.h"

#include "barkit/gowl-bar-guard.h"
#include "barkit/gowl-bar-host.h"
#include "barkit/gowl-bar-layout.h"
#include "barkit/gowl-bar-panel.h"
#include "barkit/gowl-bar-panel-render.h"
#include "barkit/gowl-bar-plugin.h"
#include "barkit/gowl-bar-registry.h"
#include "barkit/gowl-bar-theme.h"
#include "barkit/gowl-bar-toast.h"

#include "bar-internal.h"
#include "bar-sysinfo.h"

#include <xkbcommon/xkbcommon-keysyms.h>

/**
 * SECTION:gowl-module-bar
 * @title: The bar module
 * @short_description: hosts the bar's plugins, panels and toasts
 *
 * The module owns everything that touches the compositor --- scene
 * buffers, pointer routing, the tick timer, the thread pool --- and
 * nothing about what any particular widget shows.  That lives in
 * plugins, shipped and third-party alike, behind the #GowlBarPlugin
 * contract.
 *
 * There are three kinds of surface here, all built the same way: a
 * cairo image blitted into a #wlr_scene_buffer.  The bar sits on the
 * TOP scene layer, a panel and the toast stack sit on OVERLAY so they
 * float above fullscreen windows --- a notification you cannot see is
 * not a notification.
 */

/* ----------------------------------------------------------------
 * wlr_buffer backed by a cairo image surface
 * ---------------------------------------------------------------- */

typedef struct {
	struct wlr_buffer base;
	guchar *pixels;
	gsize   size;
	gint    stride;
} BarBuffer;

static void
bar_buffer_destroy(struct wlr_buffer *wlr_buf)
{
	BarBuffer *buf = wl_container_of(wlr_buf, buf, base);

	g_free(buf->pixels);
	g_free(buf);
}

static bool
bar_buffer_begin_data_ptr_access(struct wlr_buffer *wlr_buf, uint32_t flags,
                                 void **data, uint32_t *format,
                                 size_t *stride)
{
	BarBuffer *buf = wl_container_of(wlr_buf, buf, base);

	*data   = buf->pixels;
	*format = DRM_FORMAT_ARGB8888;
	*stride = (size_t)buf->stride;
	return true;
}


static void
bar_buffer_end_data_ptr_access(struct wlr_buffer *wlr_buf)
{
	(void)wlr_buf;
}

static const struct wlr_buffer_impl bar_buffer_impl = {
	.destroy = bar_buffer_destroy,
	.begin_data_ptr_access = bar_buffer_begin_data_ptr_access,
	.end_data_ptr_access = bar_buffer_end_data_ptr_access,
};

/* Wrap a finished cairo surface in a wlr_buffer.  Takes a copy: the
   scene graph holds the buffer past the point where the cairo surface
   is destroyed. */
static BarBuffer *
bar_buffer_from_cairo(cairo_surface_t *cs, gint width, gint height)
{
	BarBuffer *buf;
	gint stride;

	cairo_surface_flush(cs);
	stride = cairo_image_surface_get_stride(cs);

	buf = g_new0(BarBuffer, 1);
	buf->stride = stride;
	buf->size   = (gsize)stride * (gsize)height;
	buf->pixels = g_malloc(buf->size);
	memcpy(buf->pixels, cairo_image_surface_get_data(cs), buf->size);
	wlr_buffer_init(&buf->base, &bar_buffer_impl, width, height);
	return buf;
}

/* ----------------------------------------------------------------
 * Module state
 * ---------------------------------------------------------------- */

#define GOWL_TYPE_MODULE_BAR (gowl_module_bar_get_type())
G_DECLARE_FINAL_TYPE(GowlModuleBar, gowl_module_bar,
                     GOWL, MODULE_BAR, GowlModule)

/**
 * GowlBarPosition:
 * @GOWL_BAR_POSITION_TOP: flush with the monitor's top edge.
 * @GOWL_BAR_POSITION_BOTTOM: flush with the bottom edge.
 * @GOWL_BAR_POSITION_COUNT: sentinel.
 *
 * Two slots is the deliberate limit: stacking more bars on one edge
 * does not fit the screen budget of a tiling session.
 */
typedef enum {
	GOWL_BAR_POSITION_TOP = 0,
	GOWL_BAR_POSITION_BOTTOM = 1,
	GOWL_BAR_POSITION_COUNT
} GowlBarPosition;

/* One plugin instance placed in a bar slot. */
typedef struct {
	GowlBarPlugin *plugin;
	gchar         *spec;
	GowlBarRegion  region;
	GowlBarSlot    slot;          /* filled by the last layout pass */
	time_t         last_poll;
	volatile gint  async_inflight;
	gulong         changed_id;
	gulong         panel_changed_id;
} BarItem;

/* Per-monitor scene surface for one bar slot. */
typedef struct {
	struct wlr_scene_buffer *scene_buf;
	gint   width;
	gint   height;
	gint   mon_x;
	gint   mon_y;
	gchar *last_signature;
} BarSurface;

/* One bar slot: top or bottom. */
typedef struct {
	GowlBarPosition position;
	gboolean        enabled;
	gboolean        visible;
	gint            bar_height;

	/* TRUE while the slot still carries the layout the module ships
	   rather than one the user asked for.  The first configuration
	   that names any widget list clears it --- see
	   bar_configure_slot(). */
	gboolean        defaults_pending;

	GowlBarTheme   *theme;

	GPtrArray      *items;      /* BarItem*, owned */
	gint            anchor;     /* index of the centre anchor, or -1 */
	gchar          *anchor_id;  /* what the config asked for */

	GHashTable     *surfaces;   /* monitor name -> BarSurface* */
} GowlBarInstance;

/* The one open dropdown. */
typedef struct {
	GowlBarInstance *bar;
	BarItem         *item;         /* borrowed */
	GowlBarPanel    *panel;        /* owned */
	gpointer         monitor;      /* GowlMonitor*, borrowed */

	struct wlr_scene_buffer *scene_buf;
	gint    surf_x, surf_y;        /* monitor-local surface origin */
	gint    surf_w, surf_h;
	gint    frame_x, frame_y;      /* frame origin within the surface */
	gint    frame_w, frame_h;
	gint    content_h;
	gint    scroll;

	gint    hover_item, hover_child;
	gint    focus_item, focus_child;
	GArray *hits;                  /* GowlBarHitRect */

	gboolean drag_active;
	gchar   *drag_id;
	gint     drag_x, drag_w;
	gchar   *dirty_signature;
} BarPanelState;

/* The toast overlay, drawn on one monitor at a time. */
typedef struct {
	struct wlr_scene_buffer *scene_buf;
	gpointer monitor;
	gint     surf_x, surf_y, surf_w, surf_h;
	GArray  *hits;                 /* GowlBarHitRect, id = toast id */
	gchar   *last_signature;
} BarToastLayer;

struct _GowlModuleBar {
	GowlModule parent_instance;

	GowlBarInstance   bars[GOWL_BAR_POSITION_COUNT];
	GowlBarRegistry  *registry;
	GowlBarToastStack *toasts;
	BarSysinfo       *sysinfo;
	BarEnv            env;

	gpointer          compositor;
	gulong            focus_handler_id;
	gulong            client_added_id;
	gulong            client_removed_id;
	struct wl_event_source *tick_timer;

	BarPanelState     panel;
	BarToastLayer     toast_layer;

	/* A scratch cairo context kept alive purely so plugins can be
	   measured without a real surface; recreated only if it is lost. */
	cairo_surface_t  *measure_surface;
	cairo_t          *measure_cr;
	PangoLayout      *measure_layout;

	GThreadPool      *worker_pool;

	gchar            *state_dir;
	gchar            *plugin_dir;
	GStrv             plugin_dirs;
	GHashTable       *scanned_dirs;

	/* Coalescing: a plugin may ask for a redraw from anywhere, and
	   several will in one tick.  The flag is checked once per tick
	   rather than repainting per request. */
	volatile gint     redraw_pending;
	gboolean          in_render;

	/* Elisp-driven values, addressed by name.  Kept for the widget
	   data channel the editor already uses. */
	GHashTable       *widget_data;
};

static void bar_provider_iface_init(GowlBarProviderInterface *iface);
static void bar_startup_iface_init(GowlStartupHandlerInterface *iface);
static void bar_shutdown_iface_init(GowlShutdownHandlerInterface *iface);
static void bar_host_iface_init(GowlBarHostInterface *iface);
static void bar_rebuild_plugin_search_path(GowlModuleBar *self,
                                           const gchar   *configured);
static void bar_scan_plugin_dirs(GowlModuleBar *self);
static void bar_ipc_iface_init(GowlIpcHandlerInterface *iface);
static void bar_keybind_iface_init(GowlKeybindHandlerInterface *iface);

G_DEFINE_TYPE_WITH_CODE(GowlModuleBar, gowl_module_bar, GOWL_TYPE_MODULE,
	G_IMPLEMENT_INTERFACE(GOWL_TYPE_BAR_PROVIDER, bar_provider_iface_init)
	G_IMPLEMENT_INTERFACE(GOWL_TYPE_STARTUP_HANDLER, bar_startup_iface_init)
	G_IMPLEMENT_INTERFACE(GOWL_TYPE_SHUTDOWN_HANDLER, bar_shutdown_iface_init)
	G_IMPLEMENT_INTERFACE(GOWL_TYPE_BAR_HOST, bar_host_iface_init)
	G_IMPLEMENT_INTERFACE(GOWL_TYPE_IPC_HANDLER, bar_ipc_iface_init)
	G_IMPLEMENT_INTERFACE(GOWL_TYPE_KEYBIND_HANDLER, bar_keybind_iface_init))

/* Forward declarations for the mutual recursion between rendering,
   panel management and input. */
static void bar_redraw_all(GowlModuleBar *self);
static void bar_panel_close(GowlModuleBar *self);
static void bar_panel_open(GowlModuleBar *self, GowlBarInstance *bar,
                           BarItem *item, gpointer monitor);
static void bar_panel_rebuild(GowlModuleBar *self);
static void bar_panel_render(GowlModuleBar *self);
static void bar_toast_render(GowlModuleBar *self);
static gint bar_tick_ms(GowlModuleBar *self);

/* ----------------------------------------------------------------
 * Shared environment for the shipped plugins
 * ---------------------------------------------------------------- */

static const BarEnv *bar_shared_env;

/**
 * bar_env:
 *
 * Returns: (transfer none) (nullable): the shared environment
 */
const BarEnv *
bar_env(void)
{
	return bar_shared_env;
}

/**
 * bar_env_set:
 * @env: (nullable): the environment
 */
void
bar_env_set(const BarEnv *env)
{
	bar_shared_env = env;
}

/* ----------------------------------------------------------------
 * Subprocess helpers
 * ---------------------------------------------------------------- */

/**
 * bar_run_argv:
 * @argv: (array zero-terminated=1): the command and its arguments
 *
 * Returns: (transfer full) (nullable): the command's standard output
 */
gchar *
bar_run_argv(const gchar * const *argv)
{
	gchar *out = NULL;
	gint status = 0;

	if (argv == NULL || argv[0] == NULL)
		return NULL;

	if (!g_spawn_sync(NULL, (gchar **)argv, NULL,
	                  G_SPAWN_SEARCH_PATH | G_SPAWN_STDERR_TO_DEV_NULL,
	                  NULL, NULL, &out, NULL, &status, NULL)) {
		g_free(out);
		return NULL;
	}
	if (!g_spawn_check_wait_status(status, NULL)) {
		g_free(out);
		return NULL;
	}
	if (out != NULL && out[0] == '\0') {
		g_free(out);
		return NULL;
	}
	return out;
}

/**
 * bar_run_argv_line:
 * @argv: (array zero-terminated=1): the command and its arguments
 *
 * Returns: (transfer full) (nullable): the first line of output
 */
gchar *
bar_run_argv_line(const gchar * const *argv)
{
	gchar *out;
	gchar *nl;

	out = bar_run_argv(argv);
	if (out == NULL)
		return NULL;

	nl = strchr(out, '\n');
	if (nl != NULL)
		*nl = '\0';
	g_strchomp(out);
	if (out[0] == '\0') {
		g_free(out);
		return NULL;
	}
	return out;
}

/**
 * bar_expand_tilde:
 * @path: a path that may start with `~/'
 *
 * Returns: (transfer full): the expanded path
 */
gchar *
bar_expand_tilde(const gchar *path)
{
	if (path == NULL)
		return NULL;
	if (path[0] == '~' && path[1] == '/')
		return g_build_filename(g_get_home_dir(), path + 2, NULL);
	return g_strdup(path);
}

/**
 * bar_run_shell_line:
 * @cmdline: a command line
 *
 * Returns: (transfer full) (nullable): the first line of output
 */
gchar *
bar_run_shell_line(const gchar *cmdline)
{
	g_auto(GStrv) argv = NULL;
	gint argc = 0;
	gint i;

	if (cmdline == NULL || cmdline[0] == '\0')
		return NULL;
	if (!g_shell_parse_argv(cmdline, &argc, &argv, NULL))
		return NULL;

	/* Shell parsing does not expand `~', and a bar command written by
	   hand almost always contains one. */
	for (i = 0; i < argc; i++) {
		gchar *expanded;

		expanded = bar_expand_tilde(argv[i]);
		g_free(argv[i]);
		argv[i] = expanded;
	}

	return bar_run_argv_line((const gchar * const *)argv);
}

/**
 * bar_spawn_shell:
 * @cmdline: a command line
 */
void
bar_spawn_shell(const gchar *cmdline)
{
	g_auto(GStrv) argv = NULL;
	g_autoptr(GError) error = NULL;
	gint argc = 0;
	gint i;

	if (cmdline == NULL || cmdline[0] == '\0')
		return;
	if (!g_shell_parse_argv(cmdline, &argc, &argv, &error)) {
		g_warning("gowl-bar: cannot parse command '%s': %s", cmdline,
		          error->message);
		return;
	}
	for (i = 0; i < argc; i++) {
		gchar *expanded;

		expanded = bar_expand_tilde(argv[i]);
		g_free(argv[i]);
		argv[i] = expanded;
	}

	if (!g_spawn_async(NULL, argv, NULL,
	                   G_SPAWN_SEARCH_PATH |
	                   G_SPAWN_STDOUT_TO_DEV_NULL |
	                   G_SPAWN_STDERR_TO_DEV_NULL,
	                   NULL, NULL, NULL, &error)) {
		g_warning("gowl-bar: cannot launch '%s': %s", cmdline,
		          error->message);
	}
}

gchar *
bar_app_command(const gchar *binary, const gchar *flatpak_id,
                const gchar *args)
{
	const gchar *extra = (args != NULL) ? args : "";

	if (binary == NULL)
		return NULL;

	if (flatpak_id == NULL)
		return g_strdup_printf("%s %s", binary, extra);

	/*
	 * Native first, then the user flatpak, then the system one.
	 *
	 * The choice is made by the SHELL at launch rather than probed
	 * here, on purpose: a panel button runs on the compositor's
	 * dispatch thread while it holds cmacs_gowl_mutex, so asking
	 * `flatpak info' which form is installed would block the editor
	 * for the length of a subprocess -- the same mistake that stopped
	 * windows mapping.  The spawn is already detached, so the test
	 * costs nothing here and stays right if the user installs or
	 * removes either form later.
	 *
	 * `flatpak run' on an id that is not installed fails rather than
	 * hanging, so chaining with || needs no probe of its own.  The
	 * --user attempt comes first because that is how these desktop
	 * helpers are normally installed.
	 */
	return g_strdup_printf(
		"sh -c 'if command -v %s >/dev/null 2>&1; then exec %s %s; fi; "
		"flatpak run --user %s %s 2>/dev/null || "
		"exec flatpak run %s %s'",
		binary, binary, extra,
		flatpak_id, extra,
		flatpak_id, extra);
}

/**
 * bar_have_command:
 * @name: an executable name
 *
 * Returns: %TRUE when @name is on `PATH'
 */
gboolean
bar_have_command(const gchar *name)
{
	g_autofree gchar *found = NULL;

	if (name == NULL || name[0] == '\0')
		return FALSE;
	found = g_find_program_in_path(name);
	return (found != NULL);
}

/* ----------------------------------------------------------------
 * Worker pool
 * ---------------------------------------------------------------- */

typedef struct {
	GowlModuleBar   *module;
	GowlBarPlugin   *plugin;      /* strong ref: the item may be
	                                 replaced while the work runs */
	GowlBarWorkFunc  func;
	gpointer         user_data;
	GDestroyNotify   destroy;
	volatile gint   *inflight;    /* cleared when done, may be NULL */
} BarWork;

/* The guard takes one opaque pointer, so a two-argument callback has
   to travel in a struct rather than through a function-pointer cast:
   calling through an incompatible type is undefined even when the
   arguments happen to line up. */
static void
bar_work_body(gpointer data)
{
	BarWork *work = data;

	work->func(work->plugin, work->user_data);
}

static void
bar_work_run(gpointer data, gpointer user_data)
{
	BarWork *work = data;

	(void)user_data;

	if (work->func != NULL && work->plugin != NULL) {
		/* Under the guard: a third-party plugin's async poll is
		   still the compositor's process, and a fault here would
		   end the session just as surely as one in a draw. */
		gint signo = 0;

		if (!gowl_bar_guard_call(bar_work_body, work, &signo,
		                         "a bar plugin's background poll")) {
			g_warning("gowl-bar: plugin '%s' faulted while "
			          "polling in the background",
			          gowl_bar_plugin_get_id(work->plugin));
		}
	}

	if (work->inflight != NULL)
		g_atomic_int_set(work->inflight, 0);
	if (work->destroy != NULL && work->user_data != NULL)
		work->destroy(work->user_data);
	g_clear_object(&work->plugin);
	g_free(work);
}

/* The plugin's own poll_async, adapted to the generic work signature so
   both paths run through one queue. */
static void
bar_poll_async_trampoline(GowlBarPlugin *plugin, gpointer user_data)
{
	(void)user_data;
	gowl_bar_plugin_poll_async(plugin);
}

static void
bar_queue_work(GowlModuleBar *self, GowlBarPlugin *plugin,
               GowlBarWorkFunc func, gpointer user_data,
               GDestroyNotify destroy, volatile gint *inflight)
{
	BarWork *work;

	if (self->worker_pool == NULL || plugin == NULL) {
		if (destroy != NULL && user_data != NULL)
			destroy(user_data);
		if (inflight != NULL)
			g_atomic_int_set(inflight, 0);
		return;
	}

	work = g_new0(BarWork, 1);
	work->module    = self;
	work->plugin    = g_object_ref(plugin);
	work->func      = func;
	work->user_data = user_data;
	work->destroy   = destroy;
	work->inflight  = inflight;

	if (!g_thread_pool_push(self->worker_pool, work, NULL)) {
		g_clear_object(&work->plugin);
		if (destroy != NULL && user_data != NULL)
			destroy(user_data);
		if (inflight != NULL)
			g_atomic_int_set(inflight, 0);
		g_free(work);
	}
}

/* ----------------------------------------------------------------
 * Measurement context
 * ---------------------------------------------------------------- */

/* A 1x1 image surface is enough to own a PangoLayout: measurement
   never touches the target, and creating one per render would allocate
   a font map on every tick. */
static PangoLayout *
bar_measure_layout(GowlModuleBar *self)
{
	if (self->measure_layout != NULL)
		return self->measure_layout;

	self->measure_surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
	                                                   1, 1);
	self->measure_cr      = cairo_create(self->measure_surface);
	self->measure_layout  = pango_cairo_create_layout(self->measure_cr);
	return self->measure_layout;
}

/* ----------------------------------------------------------------
 * Items
 * ---------------------------------------------------------------- */

static void bar_on_plugin_changed(GowlBarPlugin *plugin, gpointer user_data);
static void bar_on_plugin_panel_changed(GowlBarPlugin *plugin,
                                        gpointer user_data);

static BarItem *
bar_item_new(GowlModuleBar *self, GowlBarPlugin *plugin, const gchar *spec,
             GowlBarRegion region)
{
	BarItem *item;

	item = g_new0(BarItem, 1);
	item->plugin = plugin;
	item->spec   = g_strdup(spec);
	item->region = region;

	item->changed_id = g_signal_connect(plugin, "changed",
		G_CALLBACK(bar_on_plugin_changed), self);
	item->panel_changed_id = g_signal_connect(plugin, "panel-changed",
		G_CALLBACK(bar_on_plugin_panel_changed), self);

	return item;
}

static void
bar_item_free(gpointer data)
{
	BarItem *item = data;

	if (item == NULL)
		return;

	if (item->plugin != NULL) {
		if (item->changed_id != 0)
			g_signal_handler_disconnect(item->plugin,
			                            item->changed_id);
		if (item->panel_changed_id != 0)
			g_signal_handler_disconnect(item->plugin,
			                            item->panel_changed_id);
		gowl_bar_plugin_deactivate(item->plugin);
		gowl_bar_plugin_set_host(item->plugin, NULL);
		g_object_unref(item->plugin);
	}
	g_free(item->spec);
	g_free(item);
}

/* ----------------------------------------------------------------
 * Bar rendering
 * ---------------------------------------------------------------- */

/* Lay a slot out for a given width.  Fills every item's `slot' and
   returns the anchor index actually used. */
static void
bar_layout_slot(GowlModuleBar *self, GowlBarInstance *bar, gint width)
{
	g_autofree gint *widths = NULL;
	g_autofree GowlBarRegion *regions = NULL;
	g_autofree GowlBarSlot *slots = NULL;
	PangoLayout *layout;
	guint n, i;
	gint pad, gap;

	n = bar->items->len;
	if (n == 0)
		return;

	layout  = bar_measure_layout(self);
	widths  = g_new0(gint, n);
	regions = g_new0(GowlBarRegion, n);
	slots   = g_new0(GowlBarSlot, n);

	pad = gowl_bar_theme_metric(bar->theme, GOWL_BAR_METRIC_ITEM_GAP);
	gap = gowl_bar_theme_metric(bar->theme, GOWL_BAR_METRIC_ITEM_GAP);

	for (i = 0; i < n; i++) {
		BarItem *item = g_ptr_array_index(bar->items, i);

		widths[i]  = gowl_bar_plugin_measure(item->plugin, layout,
		                                     bar->theme,
		                                     bar->bar_height);
		if (widths[i] < 0)
			widths[i] = 0;
		regions[i] = item->region;
	}

	gowl_bar_layout_run(width, pad, gap, widths, regions, (gint)n,
	                    bar->anchor, slots);

	for (i = 0; i < n; i++) {
		BarItem *item = g_ptr_array_index(bar->items, i);

		item->slot = slots[i];
	}
}

static BarBuffer *
bar_render_slot(GowlModuleBar *self, GowlBarInstance *bar, gint width,
                gint height)
{
	cairo_surface_t *cs;
	cairo_t *cr;
	PangoLayout *layout;
	BarBuffer *buf;
	guint i;

	cs = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
	cr = cairo_create(cs);

	gowl_bar_theme_cairo_set(bar->theme, cr, GOWL_BAR_COLOR_BASE);
	cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
	cairo_paint(cr);
	cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

	layout = pango_cairo_create_layout(cr);

	bar_layout_slot(self, bar, width);

	for (i = 0; i < bar->items->len; i++) {
		BarItem *item = g_ptr_array_index(bar->items, i);
		gboolean hovered;
		gboolean panel_open;

		if (!item->slot.visible || item->slot.width <= 0)
			continue;

		hovered    = FALSE;
		panel_open = (self->panel.item == item);

		gowl_bar_plugin_draw(item->plugin, cr, layout, bar->theme,
		                     item->slot.x, 0, item->slot.width,
		                     height, hovered, panel_open);
	}

	g_object_unref(layout);
	buf = bar_buffer_from_cairo(cs, width, height);
	cairo_destroy(cr);
	cairo_surface_destroy(cs);
	return buf;
}

/* Everything that changes the bar's pixels, as one string.  A tick
   whose signature matches the last one skips the cairo pass and the
   scene-buffer swap, which is what keeps an idle bar from forcing a
   compositor repaint every few seconds. */
static gchar *
bar_slot_signature(GowlModuleBar *self, GowlBarInstance *bar, gint width,
                   gint height)
{
	GString *s;
	guint i;

	s = g_string_sized_new(256);
	g_string_append_printf(s, "g:%dx%d;a:%d;", width, height, bar->anchor);

	for (i = 0; i < bar->items->len; i++) {
		BarItem *item = g_ptr_array_index(bar->items, i);

		g_string_append_printf(s, "r%d:", (gint)item->region);
		gowl_bar_plugin_signature(item->plugin, s);
		if (self->panel.item == item)
			g_string_append(s, "*open*;");
	}
	return g_string_free(s, FALSE);
}

static gint
bar_surface_y(GowlBarInstance *bar, gint mon_y, gint mon_h)
{
	if (bar->position == GOWL_BAR_POSITION_BOTTOM)
		return mon_y + mon_h - bar->bar_height;
	return mon_y;
}

static void
bar_surface_free(gpointer data)
{
	BarSurface *surface = data;

	if (surface == NULL)
		return;
	if (surface->scene_buf != NULL)
		wlr_scene_node_destroy(&surface->scene_buf->node);
	g_free(surface->last_signature);
	g_free(surface);
}

static void
bar_create_surface(GowlModuleBar *self, GowlBarInstance *bar,
                   GowlMonitor *monitor)
{
	GowlCompositor *comp;
	struct wlr_scene_tree *top_layer;
	BarSurface *surface;
	BarBuffer *buf;
	const gchar *name;
	gint mon_x, mon_y, mon_w, mon_h, surf_y;

	if (!bar->enabled || !bar->visible || bar->bar_height <= 0)
		return;
	if (self->compositor == NULL)
		return;

	comp = GOWL_COMPOSITOR(self->compositor);
	name = gowl_monitor_get_name(monitor);
	gowl_monitor_get_geometry(monitor, &mon_x, &mon_y, &mon_w, &mon_h);
	if (mon_w <= 0 || mon_h <= 0)
		return;

	g_hash_table_remove(bar->surfaces, name);

	top_layer = gowl_compositor_get_scene_layer(comp,
	                                            GOWL_SCENE_LAYER_TOP);
	if (top_layer == NULL)
		return;

	buf    = bar_render_slot(self, bar, mon_w, bar->bar_height);
	surf_y = bar_surface_y(bar, mon_y, mon_h);

	surface = g_new0(BarSurface, 1);
	surface->scene_buf = wlr_scene_buffer_create(top_layer, &buf->base);
	surface->width  = mon_w;
	surface->height = bar->bar_height;
	surface->mon_x  = mon_x;
	surface->mon_y  = surf_y;

	wlr_scene_node_set_position(&surface->scene_buf->node, mon_x, surf_y);
	/* wlroots' damage tracking otherwise leaves a second surface in
	   the same layer added-but-unrendered until something else forces
	   a traversal; raising it forces that traversal now. */
	wlr_scene_node_raise_to_top(&surface->scene_buf->node);
	wlr_buffer_drop(&buf->base);

	g_hash_table_insert(bar->surfaces, g_strdup(name), surface);
}

static void
bar_instance_destroy_surfaces(GowlBarInstance *bar)
{
	if (bar->surfaces != NULL)
		g_hash_table_remove_all(bar->surfaces);
}

static void
bar_destroy_all_surfaces(GowlModuleBar *self)
{
	gint i;

	for (i = 0; i < GOWL_BAR_POSITION_COUNT; i++)
		bar_instance_destroy_surfaces(&self->bars[i]);
}

/* ----------------------------------------------------------------
 * Polling
 * ---------------------------------------------------------------- */

/* gowl_bar_plugin_poll takes a GowlBarPlugin*, the guard takes a
   gpointer; one thin wrapper rather than a cast between function
   types. */
static void
bar_poll_body(gpointer data)
{
	gowl_bar_plugin_poll(GOWL_BAR_PLUGIN(data));
}

static void
bar_poll_items(GowlModuleBar *self)
{
	time_t now;
	gint bi;
	guint i;

	now = time(NULL);
	bar_sysinfo_tick(self->sysinfo);

	for (bi = 0; bi < GOWL_BAR_POSITION_COUNT; bi++) {
		GowlBarInstance *bar = &self->bars[bi];

		if (!bar->enabled)
			continue;

		for (i = 0; i < bar->items->len; i++) {
			BarItem *item = g_ptr_array_index(bar->items, i);
			gint interval;
			gint signo = 0;

			interval = gowl_bar_plugin_get_interval(item->plugin);
			if (interval > 0 &&
			    now - item->last_poll < interval)
				continue;
			item->last_poll = now;

			if (!gowl_bar_guard_call(bar_poll_body, item->plugin,
				    &signo,
				    gowl_bar_plugin_get_id(item->plugin))) {
				gowl_bar_registry_quarantine(self->registry,
					gowl_bar_plugin_get_setting(
						item->plugin, "name"),
					"it faulted while polling");
				continue;
			}

			if (gowl_bar_plugin_wants_async(item->plugin) &&
			    g_atomic_int_compare_and_exchange(
				    &item->async_inflight, 0, 1)) {
				bar_queue_work(self, item->plugin,
				               bar_poll_async_trampoline, NULL,
				               NULL, &item->async_inflight);
			}
		}
	}
}

/* ----------------------------------------------------------------
 * Redraw
 * ---------------------------------------------------------------- */

static void
bar_redraw_all(GowlModuleBar *self)
{
	GowlCompositor *comp;
	GList *monitors, *l;
	gint bi;

	if (self->compositor == NULL)
		return;

	/* A plugin's draw may set a label, which emits ::changed, which
	   asks for a redraw.  Without this the first paint after a label
	   change would recurse once per plugin. */
	if (self->in_render)
		return;
	self->in_render = TRUE;

	comp = GOWL_COMPOSITOR(self->compositor);
	monitors = gowl_compositor_get_monitors(comp);

	for (bi = 0; bi < GOWL_BAR_POSITION_COUNT; bi++) {
		GowlBarInstance *bar = &self->bars[bi];

		if (!bar->enabled || !bar->visible || bar->bar_height <= 0)
			continue;

		for (l = monitors; l != NULL; l = l->next) {
			GowlMonitor *mon = GOWL_MONITOR(l->data);
			const gchar *name = gowl_monitor_get_name(mon);
			BarSurface *surface;
			BarBuffer *buf;
			gchar *sig;
			gint mon_x, mon_y, mon_w, mon_h, surf_y;

			/* A disabled output (a shut lid, say) keeps its
			   GowlMonitor; drawing a bar for it would stack an
			   invisible surface over the working display. */
			if (!gowl_monitor_get_enabled(mon)) {
				g_hash_table_remove(bar->surfaces, name);
				continue;
			}

			surface = g_hash_table_lookup(bar->surfaces, name);
			if (surface == NULL || surface->scene_buf == NULL) {
				bar_create_surface(self, bar, mon);
				continue;
			}

			gowl_monitor_get_geometry(mon, &mon_x, &mon_y,
			                          &mon_w, &mon_h);
			if (surface->width != mon_w ||
			    surface->height != bar->bar_height) {
				bar_create_surface(self, bar, mon);
				continue;
			}

			surf_y = bar_surface_y(bar, mon_y, mon_h);
			if (surface->mon_x != mon_x || surface->mon_y != surf_y) {
				wlr_scene_node_set_position(
					&surface->scene_buf->node, mon_x,
					surf_y);
				surface->mon_x = mon_x;
				surface->mon_y = surf_y;
			}

			sig = bar_slot_signature(self, bar, surface->width,
			                         surface->height);
			if (surface->last_signature != NULL &&
			    strcmp(sig, surface->last_signature) == 0) {
				g_free(sig);
				continue;
			}
			g_free(surface->last_signature);
			surface->last_signature = sig;

			buf = bar_render_slot(self, bar, surface->width,
			                      surface->height);
			wlr_scene_buffer_set_buffer(surface->scene_buf,
			                            &buf->base);
			wlr_buffer_drop(&buf->base);
		}
	}

	self->in_render = FALSE;
}

/* ----------------------------------------------------------------
 * Panel
 * ---------------------------------------------------------------- */

static void
bar_panel_clear_state(GowlModuleBar *self)
{
	if (self->panel.scene_buf != NULL) {
		wlr_scene_node_destroy(&self->panel.scene_buf->node);
		self->panel.scene_buf = NULL;
	}
	g_clear_object(&self->panel.panel);
	g_clear_pointer(&self->panel.hits, g_array_unref);
	g_clear_pointer(&self->panel.drag_id, g_free);
	g_clear_pointer(&self->panel.dirty_signature, g_free);
	self->panel.bar         = NULL;
	self->panel.item        = NULL;
	self->panel.monitor     = NULL;
	self->panel.scroll      = 0;
	self->panel.hover_item  = -1;
	self->panel.hover_child = -1;
	self->panel.focus_item  = -1;
	self->panel.focus_child = -1;
	self->panel.drag_active = FALSE;
}

static void
bar_panel_close(GowlModuleBar *self)
{
	BarItem *item;

	if (self->panel.item == NULL)
		return;

	item = self->panel.item;
	bar_panel_clear_state(self);
	gowl_bar_plugin_panel_closed(item->plugin);
	bar_redraw_all(self);
}

/* Ask the plugin for a fresh panel, guarded.  A plugin that faults
   building its dropdown loses the dropdown, not the session. */
typedef struct {
	GowlBarPlugin *plugin;
	GowlBarPanel  *result;
} BuildCtx;

static void
bar_build_panel_body(gpointer data)
{
	BuildCtx *ctx = data;

	ctx->result = gowl_bar_plugin_build_panel(ctx->plugin);
}

static GowlBarPanel *
bar_build_panel_guarded(GowlModuleBar *self, BarItem *item)
{
	BuildCtx ctx;
	gint signo = 0;

	ctx.plugin = item->plugin;
	ctx.result = NULL;

	if (!gowl_bar_guard_call(bar_build_panel_body, &ctx, &signo,
	                         gowl_bar_plugin_get_id(item->plugin))) {
		const gchar *name;

		name = gowl_bar_plugin_get_setting(item->plugin, "name");
		gowl_bar_registry_quarantine(self->registry,
			(name != NULL) ? name
			               : gowl_bar_plugin_get_id(item->plugin),
			"it faulted while building its panel");
		return NULL;
	}
	return ctx.result;
}

static void
bar_panel_open(GowlModuleBar *self, GowlBarInstance *bar, BarItem *item,
               gpointer monitor)
{
	GowlBarPanel *panel;

	if (item == NULL || !gowl_bar_plugin_has_panel(item->plugin))
		return;

	/* Clicking the open panel's own widget closes it; clicking a
	   different one switches, which is what a row of dropdowns is
	   expected to do. */
	if (self->panel.item == item) {
		bar_panel_close(self);
		return;
	}
	if (self->panel.item != NULL)
		bar_panel_close(self);

	panel = bar_build_panel_guarded(self, item);
	if (panel == NULL)
		return;

	self->panel.bar         = bar;
	self->panel.item        = item;
	self->panel.panel       = panel;
	self->panel.monitor     = monitor;
	self->panel.scroll      = 0;
	self->panel.hover_item  = -1;
	self->panel.hover_child = -1;
	self->panel.focus_item  = -1;
	self->panel.focus_child = -1;
	if (self->panel.hits == NULL) {
		self->panel.hits = g_array_new(FALSE, FALSE,
		                               sizeof(GowlBarHitRect));
	}

	gowl_bar_plugin_panel_opened(item->plugin);
	bar_panel_render(self);
	bar_redraw_all(self);
}

static void
bar_panel_rebuild(GowlModuleBar *self)
{
	GowlBarPanel *panel;

	if (self->panel.item == NULL)
		return;

	panel = bar_build_panel_guarded(self, self->panel.item);
	if (panel == NULL) {
		bar_panel_close(self);
		return;
	}

	g_clear_object(&self->panel.panel);
	self->panel.panel = panel;
	bar_panel_render(self);
}

/* Where the panel wants to sit: under its widget for a top bar, above
   it for a bottom one, nudged inwards so it never leaves the output. */
static void
bar_panel_place(GowlModuleBar *self, gint mon_w, gint mon_h, gint panel_w,
                gint panel_h, gint *out_x, gint *out_y)
{
	GowlBarInstance *bar = self->panel.bar;
	BarItem *item = self->panel.item;
	gint offset, margin, x, y;

	offset = gowl_bar_theme_metric(bar->theme,
	                               GOWL_BAR_METRIC_PANEL_OFFSET);
	margin = gowl_bar_theme_metric(bar->theme, GOWL_BAR_METRIC_PAD_X);

	/* Centre the panel on its widget: aligning an edge looks wrong
	   for a widget near the middle of the bar, and clamping below
	   handles the ends. */
	x = item->slot.x + item->slot.width / 2 - panel_w / 2;
	if (x + panel_w > mon_w - margin)
		x = mon_w - margin - panel_w;
	if (x < margin)
		x = margin;

	if (bar->position == GOWL_BAR_POSITION_BOTTOM)
		y = mon_h - bar->bar_height - offset - panel_h;
	else
		y = bar->bar_height + offset;

	if (y < margin)
		y = margin;
	if (y + panel_h > mon_h - margin)
		y = mon_h - margin - panel_h;

	*out_x = x;
	*out_y = y;
}

static void
bar_panel_render(GowlModuleBar *self)
{
	GowlCompositor *comp;
	struct wlr_scene_tree *overlay;
	GowlBarPanelRenderCtx ctx;
	cairo_surface_t *cs;
	cairo_t *cr;
	PangoLayout *layout;
	PangoFontDescription *font;
	BarBuffer *buf;
	GowlBarInstance *bar;
	gint mon_x, mon_y, mon_w, mon_h;
	gint panel_w, panel_h, content_h, max_h;
	gint shadow, radius;
	gint px, py;

	if (self->panel.item == NULL || self->panel.panel == NULL)
		return;
	if (self->compositor == NULL || self->panel.monitor == NULL)
		return;

	bar  = self->panel.bar;
	comp = GOWL_COMPOSITOR(self->compositor);
	gowl_monitor_get_geometry(GOWL_MONITOR(self->panel.monitor),
	                          &mon_x, &mon_y, &mon_w, &mon_h);

	panel_w = gowl_bar_panel_get_width(self->panel.panel);
	if (panel_w <= 0)
		panel_w = gowl_bar_theme_metric(bar->theme,
		                                GOWL_BAR_METRIC_PANEL_WIDTH);
	if (panel_w > mon_w - 2 * gowl_bar_theme_metric(bar->theme,
	                                GOWL_BAR_METRIC_PAD_X))
		panel_w = mon_w - 2 * gowl_bar_theme_metric(bar->theme,
		                                GOWL_BAR_METRIC_PAD_X);
	if (panel_w < 120)
		panel_w = 120;

	layout    = bar_measure_layout(self);
	font      = pango_font_description_from_string(
			gowl_bar_theme_get_font(bar->theme));
	pango_layout_set_font_description(layout, font);
	pango_font_description_free(font);

	content_h = gowl_bar_panel_measure(self->panel.panel, layout,
	                                   bar->theme, panel_w);

	max_h = gowl_bar_panel_get_max_height(self->panel.panel);
	if (max_h <= 0)
		max_h = mon_h - bar->bar_height -
		        4 * gowl_bar_theme_metric(bar->theme,
		                                  GOWL_BAR_METRIC_PAD_Y);
	if (max_h < 80)
		max_h = 80;

	panel_h = (content_h < max_h) ? content_h : max_h;
	self->panel.content_h = content_h;

	if (self->panel.scroll > content_h - panel_h)
		self->panel.scroll = content_h - panel_h;
	if (self->panel.scroll < 0)
		self->panel.scroll = 0;

	shadow = gowl_bar_theme_metric(bar->theme, GOWL_BAR_METRIC_SHADOW);
	radius = gowl_bar_theme_metric(bar->theme, GOWL_BAR_METRIC_RADIUS);

	bar_panel_place(self, mon_w, mon_h, panel_w, panel_h, &px, &py);

	/* The surface is larger than the frame so the drop shadow has
	   somewhere to land; the frame sits inset by the shadow spread. */
	self->panel.frame_x = shadow;
	self->panel.frame_y = shadow;
	self->panel.frame_w = panel_w;
	self->panel.frame_h = panel_h;
	self->panel.surf_x  = px - shadow;
	self->panel.surf_y  = py - shadow;
	self->panel.surf_w  = panel_w + 2 * shadow;
	self->panel.surf_h  = panel_h + 2 * shadow;

	if (self->panel.surf_x < 0)
		self->panel.surf_x = 0;
	if (self->panel.surf_y < 0)
		self->panel.surf_y = 0;

	cs = cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
	                                self->panel.surf_w,
	                                self->panel.surf_h);
	cr = cairo_create(cs);

	cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
	cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.0);
	cairo_paint(cr);
	cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

	gowl_bar_panel_draw_frame(cr, bar->theme, self->panel.frame_x,
	                          self->panel.frame_y, panel_w, panel_h,
	                          NULL);

	cairo_save(cr);
	gowl_bar_cairo_rounded_rect(cr, self->panel.frame_x,
	                            self->panel.frame_y, panel_w, panel_h,
	                            radius);
	cairo_clip(cr);
	cairo_translate(cr, self->panel.frame_x, self->panel.frame_y);

	{
		PangoLayout *draw_layout;

		draw_layout = pango_cairo_create_layout(cr);

		gowl_bar_panel_render_ctx_init(&ctx, panel_w);
		ctx.max_height  = panel_h;
		ctx.scroll      = self->panel.scroll;
		ctx.hover_item  = self->panel.hover_item;
		ctx.hover_child = self->panel.hover_child;
		ctx.focus_item  = self->panel.focus_item;
		ctx.focus_child = self->panel.focus_child;
		ctx.hits        = self->panel.hits;

		gowl_bar_panel_render(self->panel.panel, cr, draw_layout,
		                      bar->theme, &ctx);
		g_object_unref(draw_layout);
	}
	cairo_restore(cr);

	/* Scroll indicator: without one, a panel taller than the screen
	   looks like a panel that is simply missing its last rows. */
	if (content_h > panel_h) {
		gdouble track_h, thumb_h, thumb_y;

		track_h = (gdouble)panel_h - 2.0 * (gdouble)radius;
		thumb_h = track_h * (gdouble)panel_h / (gdouble)content_h;
		if (thumb_h < 18.0)
			thumb_h = 18.0;
		thumb_y = (gdouble)self->panel.frame_y + (gdouble)radius +
		          (track_h - thumb_h) *
		          (gdouble)self->panel.scroll /
		          (gdouble)(content_h - panel_h);

		gowl_bar_cairo_rounded_rect(cr,
			(gdouble)(self->panel.frame_x + panel_w) - 5.0,
			thumb_y, 3.0, thumb_h, 1.5);
		gowl_bar_theme_cairo_set_alpha(bar->theme, cr,
		                               GOWL_BAR_COLOR_OVERLAY, 0.7);
		cairo_fill(cr);
	}

	buf = bar_buffer_from_cairo(cs, self->panel.surf_w,
	                            self->panel.surf_h);
	cairo_destroy(cr);
	cairo_surface_destroy(cs);

	overlay = gowl_compositor_get_scene_layer(comp,
	                                          GOWL_SCENE_LAYER_OVERLAY);
	if (overlay == NULL) {
		wlr_buffer_drop(&buf->base);
		return;
	}

	if (self->panel.scene_buf == NULL) {
		self->panel.scene_buf = wlr_scene_buffer_create(overlay,
		                                                &buf->base);
	} else {
		wlr_scene_buffer_set_buffer(self->panel.scene_buf, &buf->base);
	}
	wlr_scene_node_set_position(&self->panel.scene_buf->node,
	                            mon_x + self->panel.surf_x,
	                            mon_y + self->panel.surf_y);
	wlr_scene_node_raise_to_top(&self->panel.scene_buf->node);
	wlr_buffer_drop(&buf->base);
}

/* Translate a monitor-local point into the panel's content
   coordinates.  Returns FALSE when the point is outside the frame. */
static gboolean
bar_panel_point(GowlModuleBar *self, gint x, gint y, gint *out_x, gint *out_y)
{
	gint lx, ly;

	if (self->panel.item == NULL)
		return FALSE;

	lx = x - self->panel.surf_x - self->panel.frame_x;
	ly = y - self->panel.surf_y - self->panel.frame_y;

	if (lx < 0 || ly < 0 || lx >= self->panel.frame_w ||
	    ly >= self->panel.frame_h)
		return FALSE;

	if (out_x != NULL)
		*out_x = lx;
	if (out_y != NULL)
		*out_y = ly;
	return TRUE;
}

/* Deliver a panel action to the plugin under the fault guard. */
typedef struct {
	GowlBarPlugin *plugin;
	const gchar   *item_id;
	gint           index;
	gdouble        value;
	guint          button;
} ActionCtx;

static void
bar_panel_action_body(gpointer data)
{
	ActionCtx *ctx = data;

	gowl_bar_plugin_panel_action(ctx->plugin, ctx->item_id, ctx->index,
	                            ctx->value, ctx->button);
}

static void
bar_panel_deliver(GowlModuleBar *self, const gchar *item_id, gint index,
                  gdouble value, guint button)
{
	ActionCtx ctx;
	gint signo = 0;
	BarItem *item;

	item = self->panel.item;
	if (item == NULL || item_id == NULL)
		return;

	ctx.plugin  = item->plugin;
	ctx.item_id = item_id;
	ctx.index   = index;
	ctx.value   = value;
	ctx.button  = button;

	if (!gowl_bar_guard_call(bar_panel_action_body, &ctx, &signo,
	                         gowl_bar_plugin_get_id(item->plugin))) {
		const gchar *name;

		name = gowl_bar_plugin_get_setting(item->plugin, "name");
		gowl_bar_registry_quarantine(self->registry,
			(name != NULL) ? name
			               : gowl_bar_plugin_get_id(item->plugin),
			"it faulted handling a panel action");
		bar_panel_close(self);
		return;
	}

	bar_panel_rebuild(self);
	bar_redraw_all(self);
}

/* ----------------------------------------------------------------
 * Toasts
 * ---------------------------------------------------------------- */

/* Draw one toast card and record its hit region.  Returns its height. */
static gint
bar_toast_draw_card(GowlModuleBar *self, cairo_t *cr, PangoLayout *layout,
                    const GowlBarTheme *theme, GowlBarToast *toast,
                    gint x, gint y, gint width)
{
	PangoFontDescription *font;
	PangoRectangle logical;
	GowlBarColor accent_role;
	const gdouble *accent;
	gint pad, icon_w, text_x, text_w, height, cursor_y;
	gint base_size;

	pad = gowl_bar_theme_metric(theme, GOWL_BAR_METRIC_PAD_X);
	font = pango_font_description_from_string(
		gowl_bar_theme_get_font(theme));
	base_size = pango_font_description_get_size(font);
	if (base_size <= 0)
		base_size = 11 * PANGO_SCALE;

	accent_role = gowl_bar_toast_urgency_color(
		gowl_bar_toast_get_urgency(toast));
	accent = gowl_bar_theme_color(theme, accent_role);

	/* Measure first: the card's height is whatever its text needs,
	   and a fixed height truncates exactly the long message that
	   most needed reading. */
	icon_w = 0;
	if (gowl_bar_toast_get_icon(toast) != NULL) {
		PangoFontDescription *icon_font;

		icon_font = pango_font_description_from_string(
			gowl_bar_theme_get_icon_font(theme));
		pango_font_description_set_size(icon_font,
			(gint)((gdouble)base_size * 1.6));
		pango_layout_set_font_description(layout, icon_font);
		pango_layout_set_width(layout, -1);
		pango_layout_set_text(layout, gowl_bar_toast_get_icon(toast),
		                      -1);
		pango_layout_get_pixel_extents(layout, NULL, &logical);
		icon_w = logical.width + pad;
		pango_font_description_free(icon_font);
	}

	text_x = x + pad + icon_w;
	text_w = width - 2 * pad - icon_w;
	if (text_w < 40)
		text_w = 40;

	height = pad;

	pango_font_description_set_weight(font, PANGO_WEIGHT_BOLD);
	pango_layout_set_font_description(layout, font);
	pango_layout_set_width(layout, text_w * PANGO_SCALE);
	pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);
	pango_layout_set_text(layout, gowl_bar_toast_get_summary(toast), -1);
	pango_layout_get_pixel_extents(layout, NULL, &logical);
	height += logical.height;

	if (gowl_bar_toast_get_body(toast) != NULL) {
		pango_font_description_set_weight(font, PANGO_WEIGHT_NORMAL);
		pango_font_description_set_size(font,
			(gint)((gdouble)base_size * 0.92));
		pango_layout_set_font_description(layout, font);
		pango_layout_set_text(layout, gowl_bar_toast_get_body(toast),
		                      -1);
		pango_layout_get_pixel_extents(layout, NULL, &logical);
		height += 4 + logical.height;
	}
	if (gowl_bar_toast_get_hint(toast) != NULL)
		height += 4 + (base_size / PANGO_SCALE) + 4;

	height += pad;
	if (height < 3 * pad)
		height = 3 * pad;

	/* Now draw. */
	gowl_bar_panel_draw_frame(cr, theme, x, y, width, height, accent);

	cursor_y = y + pad;

	if (gowl_bar_toast_get_icon(toast) != NULL) {
		PangoFontDescription *icon_font;

		icon_font = pango_font_description_from_string(
			gowl_bar_theme_get_icon_font(theme));
		pango_font_description_set_size(icon_font,
			(gint)((gdouble)base_size * 1.6));
		pango_layout_set_font_description(layout, icon_font);
		pango_layout_set_width(layout, -1);
		pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);
		pango_layout_set_text(layout, gowl_bar_toast_get_icon(toast),
		                      -1);
		cairo_set_source_rgba(cr, accent[0], accent[1], accent[2],
		                      accent[3]);
		cairo_move_to(cr, x + pad, cursor_y);
		pango_cairo_show_layout(cr, layout);
		pango_font_description_free(icon_font);
	}

	pango_font_description_set_size(font, base_size);
	pango_font_description_set_weight(font, PANGO_WEIGHT_BOLD);
	pango_layout_set_font_description(layout, font);
	pango_layout_set_width(layout, text_w * PANGO_SCALE);
	pango_layout_set_text(layout, gowl_bar_toast_get_summary(toast), -1);
	pango_layout_get_pixel_extents(layout, NULL, &logical);
	gowl_bar_theme_cairo_set(theme, cr, GOWL_BAR_COLOR_TEXT);
	cairo_move_to(cr, text_x, cursor_y);
	pango_cairo_show_layout(cr, layout);
	cursor_y += logical.height;

	if (gowl_bar_toast_get_body(toast) != NULL) {
		pango_font_description_set_weight(font, PANGO_WEIGHT_NORMAL);
		pango_font_description_set_size(font,
			(gint)((gdouble)base_size * 0.92));
		pango_layout_set_font_description(layout, font);
		pango_layout_set_text(layout, gowl_bar_toast_get_body(toast),
		                      -1);
		pango_layout_get_pixel_extents(layout, NULL, &logical);
		gowl_bar_theme_cairo_set(theme, cr, GOWL_BAR_COLOR_SUBTEXT);
		cairo_move_to(cr, text_x, cursor_y + 4);
		pango_cairo_show_layout(cr, layout);
		cursor_y += 4 + logical.height;
	}

	if (gowl_bar_toast_get_hint(toast) != NULL) {
		pango_font_description_set_size(font,
			(gint)((gdouble)base_size * 0.82));
		pango_layout_set_font_description(layout, font);
		pango_layout_set_text(layout, gowl_bar_toast_get_hint(toast),
		                      -1);
		cairo_set_source_rgba(cr, accent[0], accent[1], accent[2],
		                      accent[3] * 0.9);
		cairo_move_to(cr, text_x, cursor_y + 4);
		pango_cairo_show_layout(cr, layout);
	}

	pango_layout_set_wrap(layout, PANGO_WRAP_WORD);
	pango_font_description_free(font);

	{
		GowlBarHitRect rect;

		rect.item_index  = (gint)gowl_bar_toast_get_id(toast);
		rect.child_index = -1;
		rect.kind        = GOWL_BAR_ITEM_ROW;
		rect.id          = NULL;
		rect.x           = x;
		rect.y           = y;
		rect.width       = width;
		rect.height      = height;
		if (self->toast_layer.hits != NULL)
			g_array_append_val(self->toast_layer.hits, rect);
	}

	return height;
}

/* The toast stack's visible content, as one string, so an unchanged
   stack does not repaint. */
static gchar *
bar_toast_signature(GowlModuleBar *self)
{
	GString *s;
	guint i, n;

	s = g_string_sized_new(128);
	n = gowl_bar_toast_stack_size(self->toasts);
	for (i = 0; i < n; i++) {
		GowlBarToast *t = gowl_bar_toast_stack_get(self->toasts, i);

		g_string_append_printf(s, "%u:%d:%s|%s|%s;",
			gowl_bar_toast_get_id(t),
			(gint)gowl_bar_toast_get_urgency(t),
			gowl_bar_toast_get_summary(t),
			(gowl_bar_toast_get_body(t) != NULL)
				? gowl_bar_toast_get_body(t) : "",
			(gowl_bar_toast_get_hint(t) != NULL)
				? gowl_bar_toast_get_hint(t) : "");
	}
	return g_string_free(s, FALSE);
}

static void
bar_toast_destroy_surface(GowlModuleBar *self)
{
	if (self->toast_layer.scene_buf != NULL) {
		wlr_scene_node_destroy(&self->toast_layer.scene_buf->node);
		self->toast_layer.scene_buf = NULL;
	}
	g_clear_pointer(&self->toast_layer.last_signature, g_free);
	self->toast_layer.monitor = NULL;
}

static void
bar_toast_render(GowlModuleBar *self)
{
	GowlCompositor *comp;
	GowlMonitor *mon;
	struct wlr_scene_tree *overlay;
	const GowlBarTheme *theme;
	cairo_surface_t *cs;
	cairo_t *cr;
	PangoLayout *layout;
	BarBuffer *buf;
	g_autofree gchar *sig = NULL;
	gint mon_x, mon_y, mon_w, mon_h;
	gint card_w, gap, margin, bar_h;
	gint total_h, y;
	guint i, n;

	if (self->compositor == NULL)
		return;

	n = gowl_bar_toast_stack_size(self->toasts);
	if (n == 0) {
		bar_toast_destroy_surface(self);
		return;
	}

	comp = GOWL_COMPOSITOR(self->compositor);
	mon  = gowl_compositor_get_selected_monitor(comp);
	if (mon == NULL) {
		GList *monitors = gowl_compositor_get_monitors(comp);

		if (monitors == NULL)
			return;
		mon = GOWL_MONITOR(monitors->data);
	}

	theme = self->bars[GOWL_BAR_POSITION_TOP].theme;
	gowl_monitor_get_geometry(mon, &mon_x, &mon_y, &mon_w, &mon_h);

	card_w = gowl_bar_theme_metric(theme, GOWL_BAR_METRIC_TOAST_WIDTH);
	gap    = gowl_bar_theme_metric(theme, GOWL_BAR_METRIC_GAP);
	margin = gowl_bar_theme_metric(theme, GOWL_BAR_METRIC_PAD_X);
	if (card_w > mon_w - 2 * margin)
		card_w = mon_w - 2 * margin;

	bar_h = 0;
	if (self->bars[GOWL_BAR_POSITION_TOP].enabled &&
	    self->bars[GOWL_BAR_POSITION_TOP].visible)
		bar_h = self->bars[GOWL_BAR_POSITION_TOP].bar_height;

	sig = bar_toast_signature(self);
	if (self->toast_layer.scene_buf != NULL &&
	    self->toast_layer.monitor == mon &&
	    self->toast_layer.last_signature != NULL &&
	    strcmp(sig, self->toast_layer.last_signature) == 0)
		return;

	if (self->toast_layer.hits == NULL) {
		self->toast_layer.hits = g_array_new(FALSE, FALSE,
		                                     sizeof(GowlBarHitRect));
	}
	g_array_set_size(self->toast_layer.hits, 0);

	/* A generous height: the cards are measured while drawing, and
	   the surface is trimmed to what they actually used. */
	total_h = (gint)n * (10 * gap + 120) + 2 * margin;
	if (total_h > mon_h)
		total_h = mon_h;

	cs = cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
	                                card_w + 2 * margin, total_h);
	cr = cairo_create(cs);
	cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
	cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.0);
	cairo_paint(cr);
	cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

	layout = pango_cairo_create_layout(cr);

	y = margin;
	for (i = 0; i < n; i++) {
		GowlBarToast *toast;
		gint h;

		toast = gowl_bar_toast_stack_get(self->toasts, i);
		if (toast == NULL)
			continue;
		if (y + 60 > total_h)
			break;
		h = bar_toast_draw_card(self, cr, layout, theme, toast,
		                        margin, y, card_w);
		y += h + gap;
	}

	g_object_unref(layout);
	buf = bar_buffer_from_cairo(cs, card_w + 2 * margin, total_h);
	cairo_destroy(cr);
	cairo_surface_destroy(cs);

	overlay = gowl_compositor_get_scene_layer(comp,
	                                          GOWL_SCENE_LAYER_OVERLAY);
	if (overlay == NULL) {
		wlr_buffer_drop(&buf->base);
		return;
	}

	self->toast_layer.surf_w = card_w + 2 * margin;
	self->toast_layer.surf_h = total_h;
	self->toast_layer.surf_x = mon_w - self->toast_layer.surf_w;
	self->toast_layer.surf_y = bar_h;
	self->toast_layer.monitor = mon;
	g_free(self->toast_layer.last_signature);
	self->toast_layer.last_signature = g_strdup(sig);

	if (self->toast_layer.scene_buf == NULL) {
		self->toast_layer.scene_buf =
			wlr_scene_buffer_create(overlay, &buf->base);
	} else {
		wlr_scene_buffer_set_buffer(self->toast_layer.scene_buf,
		                            &buf->base);
	}
	wlr_scene_node_set_position(&self->toast_layer.scene_buf->node,
	                            mon_x + self->toast_layer.surf_x,
	                            mon_y + self->toast_layer.surf_y);
	wlr_scene_node_raise_to_top(&self->toast_layer.scene_buf->node);
	wlr_buffer_drop(&buf->base);
}

/* A click on a toast: run whatever it was pointing at, then dismiss
   it.  This is the "Setup Wi-Fi" path -- a notification that hands you
   the dropdown that fixes the thing it is telling you about. */
static gboolean
bar_toast_click(GowlModuleBar *self, gint x, gint y)
{
	gint lx, ly, i;

	if (self->toast_layer.scene_buf == NULL ||
	    self->toast_layer.hits == NULL)
		return FALSE;

	lx = x - self->toast_layer.surf_x;
	ly = y - self->toast_layer.surf_y;
	if (lx < 0 || ly < 0 || lx >= self->toast_layer.surf_w ||
	    ly >= self->toast_layer.surf_h)
		return FALSE;

	i = gowl_bar_hit_find(self->toast_layer.hits, lx, ly);
	if (i < 0)
		return FALSE;

	{
		GowlBarHitRect *rect;
		GowlBarToast *toast;
		guint id;
		g_autofree gchar *panel_id = NULL;
		g_autofree gchar *command = NULL;

		rect = &g_array_index(self->toast_layer.hits, GowlBarHitRect,
		                      i);
		id = (guint)rect->item_index;

		toast = NULL;
		{
			guint j, n;

			n = gowl_bar_toast_stack_size(self->toasts);
			for (j = 0; j < n; j++) {
				GowlBarToast *t;

				t = gowl_bar_toast_stack_get(self->toasts, j);
				if (gowl_bar_toast_get_id(t) == id) {
					toast = t;
					break;
				}
			}
		}
		if (toast == NULL)
			return FALSE;

		/* Copy what the toast points at before dismissing it: the
		   dismiss frees the toast, and the panel open below would
		   otherwise read a freed string. */
		panel_id = g_strdup(gowl_bar_toast_get_panel(toast));
		command  = g_strdup(gowl_bar_toast_get_command(toast));

		gowl_bar_toast_stack_dismiss(self->toasts, id);
		bar_toast_render(self);

		if (panel_id != NULL) {
			gowl_bar_host_open_panel(GOWL_BAR_HOST(self), panel_id);
		} else if (command != NULL) {
			bar_spawn_shell(command);
		}
	}
	return TRUE;
}

/* ----------------------------------------------------------------
 * GowlBarHost
 * ---------------------------------------------------------------- */

static const GowlBarTheme *
host_get_theme(GowlBarHost *host)
{
	GowlModuleBar *self = GOWL_MODULE_BAR(host);

	return self->bars[GOWL_BAR_POSITION_TOP].theme;
}

static void
host_request_redraw(GowlBarHost *host)
{
	GowlModuleBar *self = GOWL_MODULE_BAR(host);

	/* Only a flag: a plugin may call this from a worker thread, and
	   touching the scene graph off the dispatch thread is how a
	   compositor corrupts itself.  The tick picks it up. */
	g_atomic_int_set(&self->redraw_pending, 1);
}

static void
host_request_panel_refresh(GowlBarHost *host, GowlBarPlugin *plugin)
{
	GowlModuleBar *self = GOWL_MODULE_BAR(host);

	if (self->panel.item == NULL || self->panel.item->plugin != plugin)
		return;
	g_atomic_int_set(&self->redraw_pending, 1);
	g_free(self->panel.dirty_signature);
	self->panel.dirty_signature = g_strdup("refresh");
}

static BarItem *
bar_find_item(GowlModuleBar *self, const gchar *plugin_id,
              GowlBarInstance **out_bar)
{
	gint bi;
	guint i;

	if (plugin_id == NULL)
		return NULL;

	for (bi = 0; bi < GOWL_BAR_POSITION_COUNT; bi++) {
		GowlBarInstance *bar = &self->bars[bi];

		for (i = 0; i < bar->items->len; i++) {
			BarItem *item = g_ptr_array_index(bar->items, i);
			const gchar *name;

			if (g_strcmp0(gowl_bar_plugin_get_id(item->plugin),
			              plugin_id) == 0) {
				if (out_bar != NULL)
					*out_bar = bar;
				return item;
			}
			/* Also match on the registry name, so a toast can
			   say `network' without knowing the instance was
			   configured as `network:wlan0'. */
			name = gowl_bar_plugin_get_setting(item->plugin,
			                                   "name");
			if (g_strcmp0(name, plugin_id) == 0) {
				if (out_bar != NULL)
					*out_bar = bar;
				return item;
			}
		}
	}
	return NULL;
}

static void
host_open_panel(GowlBarHost *host, const gchar *plugin_id)
{
	GowlModuleBar *self = GOWL_MODULE_BAR(host);
	GowlBarInstance *bar = NULL;
	BarItem *item;
	GowlMonitor *mon;

	item = bar_find_item(self, plugin_id, &bar);
	if (item == NULL || bar == NULL) {
		g_message("gowl-bar: no bar widget named '%s' to open",
		          (plugin_id != NULL) ? plugin_id : "(null)");
		return;
	}

	if (self->compositor == NULL)
		return;
	mon = gowl_compositor_get_selected_monitor(
		GOWL_COMPOSITOR(self->compositor));
	if (mon == NULL) {
		GList *monitors;

		monitors = gowl_compositor_get_monitors(
			GOWL_COMPOSITOR(self->compositor));
		if (monitors == NULL)
			return;
		mon = GOWL_MONITOR(monitors->data);
	}

	/* The widget must have been laid out for the panel to know where
	   to sit, and it has not been if the bar has never drawn. */
	if (item->slot.width <= 0)
		bar_redraw_all(self);

	bar_panel_open(self, bar, item, mon);
}

static void
host_close_panel(GowlBarHost *host)
{
	bar_panel_close(GOWL_MODULE_BAR(host));
}

static void
host_notify(GowlBarHost *host, GowlBarToast *toast)
{
	GowlModuleBar *self = GOWL_MODULE_BAR(host);

	gowl_bar_toast_stack_push(self->toasts, toast);
	g_atomic_int_set(&self->redraw_pending, 1);
}

static void
host_queue_work(GowlBarHost *host, GowlBarPlugin *plugin,
                GowlBarWorkFunc func, gpointer user_data,
                GDestroyNotify destroy)
{
	bar_queue_work(GOWL_MODULE_BAR(host), plugin, func, user_data,
	               destroy, NULL);
}

static void
host_spawn(GowlBarHost *host, const gchar *cmdline)
{
	(void)host;
	bar_spawn_shell(cmdline);
}

static const gchar *
host_get_state_dir(GowlBarHost *host)
{
	return GOWL_MODULE_BAR(host)->state_dir;
}

/*
 * Change one of the bar's own settings from a plugin panel.
 *
 * Routed through the same gowl_bar_theme_apply_setting() a config pass
 * uses, so a change made from a panel and one made in the config file
 * are indistinguishable -- and then the bar is re-measured, because a
 * scale or font change alters every widget's width and a redraw alone
 * would paint the new size into the old slots.
 *
 * Only `theme-*' is accepted.  A plugin reaching further into the bar's
 * configuration -- rewriting the widget list, moving regions -- is a
 * different and much larger question than "make this text bigger".
 */
static gboolean
host_set_bar_setting(GowlBarHost *host, const gchar *key, const gchar *value)
{
	GowlModuleBar *self = GOWL_MODULE_BAR(host);
	gboolean       applied = FALSE;
	gint           i;

	if (key == NULL || !g_str_has_prefix(key, "theme-"))
		return FALSE;

	for (i = 0; i < GOWL_BAR_POSITION_COUNT; i++) {
		GowlBarInstance *bar = &self->bars[i];

		if (bar->theme == NULL)
			continue;
		if (gowl_bar_theme_apply_setting(bar->theme, key, value))
			applied = TRUE;
	}

	/*
	 * A redraw is enough: bar_layout_slot() re-measures every item on
	 * every render, so the new metrics are picked up on the next frame.
	 * The bar's HEIGHT is its own config key rather than something
	 * derived from the theme, so this cannot invalidate the surface --
	 * and the size check in the per-monitor pass would rebuild it
	 * anyway if it ever did.
	 */
	if (applied)
		host_request_redraw(host);
	return applied;
}

static void
bar_host_iface_init(GowlBarHostInterface *iface)
{
	iface->get_theme             = host_get_theme;
	iface->request_redraw        = host_request_redraw;
	iface->request_panel_refresh = host_request_panel_refresh;
	iface->open_panel            = host_open_panel;
	iface->close_panel           = host_close_panel;
	iface->notify                = host_notify;
	iface->queue_work            = host_queue_work;
	iface->spawn                 = host_spawn;
	iface->get_state_dir         = host_get_state_dir;
	iface->set_bar_setting       = host_set_bar_setting;
}

/* ----------------------------------------------------------------
 * Plugin signals
 * ---------------------------------------------------------------- */

static void
bar_on_plugin_changed(GowlBarPlugin *plugin, gpointer user_data)
{
	GowlModuleBar *self = GOWL_MODULE_BAR(user_data);

	(void)plugin;
	g_atomic_int_set(&self->redraw_pending, 1);
}

static void
bar_on_plugin_panel_changed(GowlBarPlugin *plugin, gpointer user_data)
{
	GowlModuleBar *self = GOWL_MODULE_BAR(user_data);

	if (self->panel.item != NULL && self->panel.item->plugin == plugin) {
		g_atomic_int_set(&self->redraw_pending, 1);
		g_free(self->panel.dirty_signature);
		self->panel.dirty_signature = g_strdup("refresh");
	}
}

static void
bar_on_toasts_changed(GowlBarToastStack *stack, gpointer user_data)
{
	GowlModuleBar *self = GOWL_MODULE_BAR(user_data);

	(void)stack;
	g_atomic_int_set(&self->redraw_pending, 1);
}

/* A plugin leaving the registry takes every instance of it with it:
   the vtable those instances call through belongs to the entry being
   dropped. */
static void
bar_on_plugin_unloaded(GowlBarRegistry *registry, const gchar *name,
                       gpointer user_data)
{
	GowlModuleBar *self = GOWL_MODULE_BAR(user_data);
	gint bi;
	guint i;
	gboolean removed;

	(void)registry;

	removed = FALSE;
	for (bi = 0; bi < GOWL_BAR_POSITION_COUNT; bi++) {
		GowlBarInstance *bar = &self->bars[bi];

		i = 0;
		while (i < bar->items->len) {
			BarItem *item = g_ptr_array_index(bar->items, i);
			const gchar *item_name;

			item_name = gowl_bar_plugin_get_setting(item->plugin,
			                                        "name");
			if (g_strcmp0(item_name, name) != 0) {
				i++;
				continue;
			}

			if (self->panel.item == item)
				bar_panel_clear_state(self);
			if (bar->anchor == (gint)i)
				bar->anchor = -1;
			else if (bar->anchor > (gint)i)
				bar->anchor--;

			g_ptr_array_remove_index(bar->items, i);
			removed = TRUE;
		}
	}

	if (removed)
		g_atomic_int_set(&self->redraw_pending, 1);
}

/* Raised from a load, a poll or a panel action -- every one of which
   runs on a thread that owns the compositor -- so the overlay can be
   rebuilt here rather than waiting up to a tick.  A plugin-crash notice
   that appears five seconds later has already been overtaken by the
   user wondering what happened. */
static void
bar_on_plugin_failed(GowlBarRegistry *registry, const gchar *name,
                     const gchar *message, gpointer user_data)
{
	GowlModuleBar *self = GOWL_MODULE_BAR(user_data);
	GowlBarToast *toast;
	g_autofree gchar *summary = NULL;

	(void)registry;

	summary = g_strdup_printf("Bar plugin '%s' held back",
	                          (name != NULL) ? name : "(unknown)");
	toast = gowl_bar_toast_new(summary, message);
	gowl_bar_toast_set_urgency(toast, GOWL_BAR_TOAST_CRITICAL);
	gowl_bar_toast_set_icon(toast, "\xef\x81\xb1");   /* U+F071 warning */
	gowl_bar_toast_set_app(toast, "gowl bar");
	gowl_bar_toast_set_hint(toast,
		"Click to dismiss. Clear it with: gowl bar-plugin-clear "
		"<name>");
	gowl_bar_toast_stack_push(self->toasts, toast);
	g_atomic_int_set(&self->redraw_pending, 1);
	bar_toast_render(self);
}

/* ----------------------------------------------------------------
 * Configuration
 * ---------------------------------------------------------------- */

static gboolean
bar_parse_bool(const gchar *value, gboolean fallback)
{
	if (value == NULL)
		return fallback;
	if (g_ascii_strcasecmp(value, "true") == 0 ||
	    g_ascii_strcasecmp(value, "t") == 0 ||
	    g_ascii_strcasecmp(value, "1") == 0 ||
	    g_ascii_strcasecmp(value, "yes") == 0 ||
	    g_ascii_strcasecmp(value, "on") == 0)
		return TRUE;
	if (g_ascii_strcasecmp(value, "false") == 0 ||
	    g_ascii_strcasecmp(value, "nil") == 0 ||
	    g_ascii_strcasecmp(value, "0") == 0 ||
	    g_ascii_strcasecmp(value, "no") == 0 ||
	    g_ascii_strcasecmp(value, "off") == 0)
		return FALSE;
	return fallback;
}

/* Instantiate one widget spec into @bar's item list. */
static void
bar_add_item(GowlModuleBar *self, GowlBarInstance *bar, const gchar *spec,
             GowlBarRegion region)
{
	g_autoptr(GError) error = NULL;
	GowlBarPlugin *plugin;
	BarItem *item;

	if (spec == NULL || spec[0] == '\0')
		return;

	plugin = gowl_bar_registry_instantiate(self->registry, spec, &error);
	if (plugin == NULL) {
		g_warning("gowl-bar: cannot add widget '%s': %s", spec,
		          error->message);
		return;
	}

	gowl_bar_plugin_set_host(plugin, GOWL_BAR_HOST(self));
	gowl_bar_plugin_configure(plugin, NULL);

	if (!gowl_bar_plugin_activate(plugin, &error)) {
		g_warning("gowl-bar: widget '%s' declined to run: %s", spec,
		          (error != NULL) ? error->message : "no reason given");
		gowl_bar_plugin_set_host(plugin, NULL);
		g_object_unref(plugin);
		return;
	}

	item = bar_item_new(self, plugin, spec, region);
	g_ptr_array_add(bar->items, item);
}

/* Replace one region's items, keeping the other two intact.  A config
   that sets only `widgets-right' must not blank the left. */
static void
bar_set_region(GowlModuleBar *self, GowlBarInstance *bar,
               GowlBarRegion region, const gchar *spec_list)
{
	g_auto(GStrv) parts = NULL;
	GPtrArray *next;
	guint i;

	next = g_ptr_array_new_with_free_func(bar_item_free);

	for (i = 0; i < bar->items->len; i++) {
		BarItem *item = g_ptr_array_index(bar->items, i);

		if (item->region != region) {
			g_ptr_array_add(next, item);
			continue;
		}
		if (self->panel.item == item)
			bar_panel_clear_state(self);
		bar_item_free(item);
	}

	/* The survivors now belong to `next'; clearing the old array's
	   free function is what stops it freeing them a second time on
	   the way out. */
	g_ptr_array_set_free_func(bar->items, NULL);
	g_ptr_array_free(bar->items, TRUE);
	bar->items = next;

	if (spec_list != NULL) {
		parts = g_strsplit_set(spec_list, " \t", -1);
		for (i = 0; parts[i] != NULL; i++) {
			if (parts[i][0] == '\0')
				continue;
			bar_add_item(self, bar, parts[i], region);
		}
	}

	bar->anchor = -1;
}

/* Resolve the configured anchor name to an index in the current item
   list.  Re-run after any change to the items, since the index moves. */
static void
bar_resolve_anchor(GowlBarInstance *bar)
{
	guint i;

	bar->anchor = -1;
	if (bar->anchor_id == NULL)
		return;

	for (i = 0; i < bar->items->len; i++) {
		BarItem *item = g_ptr_array_index(bar->items, i);
		const gchar *name;

		if (item->region != GOWL_BAR_REGION_CENTER)
			continue;
		if (g_strcmp0(item->spec, bar->anchor_id) == 0) {
			bar->anchor = (gint)i;
			return;
		}
		name = gowl_bar_plugin_get_setting(item->plugin, "name");
		if (g_strcmp0(name, bar->anchor_id) == 0) {
			bar->anchor = (gint)i;
			return;
		}
	}
}

/* Forward a `<target>.<key>' or legacy `<plugin>-<key>' setting to the
   items it names.  @target matches either a plugin's registry name --
   so `toggle.icon' reaches every toggle -- or one item's whole spec --
   so `toggle:caffeine.icon' reaches exactly one of them.  That
   distinction is what makes two instances of the same widget
   separately configurable.

   This is also what keeps a config written against the old widget list
   -- `cpu-color', `tag-active-bg', `title-palette' -- working
   unchanged.

   Returns how many items it reached. */
static gint
bar_forward_setting(GowlModuleBar *self, GowlBarInstance *bar,
                    const gchar *target, const gchar *key,
                    const gchar *value)
{
	guint i;
	gint reached;

	(void)self;

	reached = 0;
	for (i = 0; i < bar->items->len; i++) {
		BarItem *item = g_ptr_array_index(bar->items, i);
		const gchar *name;

		name = gowl_bar_plugin_get_setting(item->plugin, "name");
		if (g_strcmp0(name, target) != 0 &&
		    g_strcmp0(item->spec, target) != 0)
			continue;
		gowl_bar_plugin_set_setting(item->plugin, key, value);
		gowl_bar_plugin_configure(item->plugin, NULL);
		reached++;
	}
	return reached;
}

/* Apply one settings map to one slot.  Split out from bar_configure so
   the `bottom-' prefix below can reuse it verbatim rather than growing
   a second, subtly different copy. */
static void
bar_configure_slot(GowlModuleBar *self, GowlBarInstance *bar,
                   GHashTable *settings)
{
	const gchar *val;
	GHashTableIter iter;
	gpointer k, v;

	/* Reaching configure at all is what marks a slot as in play; an
	   explicit `enabled: false' is how the bottom bar is turned off
	   again without deleting its whole block. */
	val = g_hash_table_lookup(settings, "enabled");
	if (val != NULL && !bar_parse_bool(val, TRUE)) {
		bar->enabled = FALSE;
		bar_instance_destroy_surfaces(bar);
		return;
	}
	bar->enabled = TRUE;

	val = g_hash_table_lookup(settings, "height");
	if (val != NULL)
		bar->bar_height = (gint)g_ascii_strtoll(val, NULL, 10);

	val = g_hash_table_lookup(settings, "visible");
	if (val != NULL) {
		gboolean want = bar_parse_bool(val, TRUE);

		bar->visible = want;
		if (!want)
			bar_instance_destroy_surfaces(bar);
	}

	/* Theme.  The legacy colour keys are applied first so an explicit
	   `theme-*' in the same call still wins. */
	val = g_hash_table_lookup(settings, "bg-color");
	if (val != NULL)
		gowl_bar_theme_set_color(bar->theme, GOWL_BAR_COLOR_BASE, val);
	val = g_hash_table_lookup(settings, "fg-color");
	if (val != NULL)
		gowl_bar_theme_set_color(bar->theme, GOWL_BAR_COLOR_TEXT, val);
	val = g_hash_table_lookup(settings, "font");
	if (val != NULL)
		gowl_bar_theme_set_font(bar->theme, val);
	val = g_hash_table_lookup(settings, "font-size");
	if (val != NULL) {
		g_autofree gchar *font = NULL;

		font = g_strdup_printf("monospace %.0f",
		                       g_ascii_strtod(val, NULL));
		gowl_bar_theme_set_font(bar->theme, font);
	}

	g_hash_table_iter_init(&iter, settings);
	while (g_hash_table_iter_next(&iter, &k, &v))
		gowl_bar_theme_apply_setting(bar->theme, (const gchar *)k,
		                             (const gchar *)v);

	/*
	 * The shipped layout is what a bar nobody has configured looks
	 * like, not a base to add to.  The first configuration naming any
	 * widget list replaces it wholesale.
	 *
	 * Without this, a configuration written before regions existed --
	 * which sets only `widgets' -- keeps the shipped centre clock
	 * alongside the clock at the end of its own list, and the bar
	 * shows the time twice.
	 */
	if (bar->defaults_pending) {
		GowlBarConfigKind kind;

		kind = gowl_bar_layout_config_kind(settings);
		if (kind != GOWL_BAR_CONFIG_NONE) {
			bar_set_region(self, bar, GOWL_BAR_REGION_LEFT, NULL);
			bar_set_region(self, bar, GOWL_BAR_REGION_CENTER, NULL);
			bar_set_region(self, bar, GOWL_BAR_REGION_RIGHT, NULL);
			g_clear_pointer(&bar->anchor_id, g_free);

			/*
			 * A `widgets' key with no region describes the bar as
			 * it was before regions: the tag row and the window
			 * title on the left, the status list on the right.
			 * Put the left back, or upgrading would silently cost
			 * every such configuration its tags and its title.
			 */
			if (kind == GOWL_BAR_CONFIG_LEGACY) {
				bar_set_region(self, bar,
				               GOWL_BAR_REGION_LEFT,
				               "tags title");
			}
			bar->defaults_pending = FALSE;
		}
	}

	/* Widget lists.  `widgets' without a region keeps its historical
	   meaning: the right-hand status list. */
	/*
	 * Plugin loading comes BEFORE the widget lists, so a plugin named
	 * in `widgets-right' is registered by the time that list is
	 * parsed.  The other order silently drops the widget: an unknown
	 * name is skipped rather than being an error, which is right for a
	 * typo and wrong for a plugin that simply had not loaded yet.
	 */
	val = g_hash_table_lookup(settings, "plugin-dir");
	if (val == NULL)
		val = g_hash_table_lookup(settings, "plugin-path");
	if (val != NULL) {
		bar_rebuild_plugin_search_path(self, val);
		/* Configuration can arrive either side of attach.  Scanning
		   again is safe -- a directory is scanned at most once -- and
		   without it a plugin-dir named in a config read after attach
		   is searched but never auto-loaded. */
		if (self->compositor != NULL)
			bar_scan_plugin_dirs(self);
	}

	val = g_hash_table_lookup(settings, "plugins");
	if (val != NULL) {
		g_auto(GStrv) names = g_strsplit_set(val, " \t,", -1);
		gint pi;

		for (pi = 0; names[pi] != NULL; pi++) {
			g_autoptr(GError) perr = NULL;
			g_autofree gchar *resolved = NULL;

			if (names[pi][0] == '\0')
				continue;
			resolved = gowl_bar_registry_resolve_file(
				self->registry, names[pi], &perr);
			if (resolved == NULL
			    || !gowl_bar_registry_load_file(self->registry,
			                                    resolved, &perr)) {
				/* Loud, because a plugin the user asked for
				   by name and did not get is not something
				   to discover from an empty space on the
				   bar. */
				g_warning("gowl-bar: %s", perr->message);
				continue;
			}
			g_message("gowl-bar: loaded plugin '%s' from %s",
			          names[pi], resolved);
		}
	}

	val = g_hash_table_lookup(settings, "widgets-left");
	if (val != NULL)
		bar_set_region(self, bar, GOWL_BAR_REGION_LEFT, val);
	val = g_hash_table_lookup(settings, "widgets-center");
	if (val == NULL)
		val = g_hash_table_lookup(settings, "widgets-centre");
	if (val != NULL)
		bar_set_region(self, bar, GOWL_BAR_REGION_CENTER, val);
	val = g_hash_table_lookup(settings, "widgets-right");
	if (val == NULL)
		val = g_hash_table_lookup(settings, "widgets");
	if (val != NULL)
		bar_set_region(self, bar, GOWL_BAR_REGION_RIGHT, val);

	val = g_hash_table_lookup(settings, "center-anchor");
	if (val == NULL)
		val = g_hash_table_lookup(settings, "centre-anchor");
	if (val != NULL) {
		g_free(bar->anchor_id);
		bar->anchor_id = (val[0] != '\0') ? g_strdup(val) : NULL;
	}
	bar_resolve_anchor(bar);

	/* Legacy per-widget keys, and the generic `<widget>.<key>' form
	   that replaces them. */
	g_hash_table_iter_init(&iter, settings);
	while (g_hash_table_iter_next(&iter, &k, &v)) {
		const gchar *key = (const gchar *)k;
		const gchar *dot;

		/* The LAST dot, so a spec that contains one --
		   `disk:/etc/foo.d.color' -- still splits into the widget
		   and the key rather than at the path. */
		dot = strrchr(key, '.');
		if (dot != NULL && dot != key) {
			g_autofree gchar *target = NULL;

			target = g_strndup(key, (gsize)(dot - key));
			if (bar_forward_setting(self, bar, target, dot + 1,
			                        (const gchar *)v) == 0) {
				const gchar *first = strchr(key, '.');

				/* Nothing matched: the widget's own name may
				   be the part before the first dot instead. */
				if (first != NULL && first != dot &&
				    first != key) {
					g_autofree gchar *head = NULL;

					head = g_strndup(key,
						(gsize)(first - key));
					bar_forward_setting(self, bar, head,
						first + 1, (const gchar *)v);
				}
			}
			continue;
		}

		if (g_str_has_suffix(key, "-color") &&
		    strcmp(key, "bg-color") != 0 &&
		    strcmp(key, "fg-color") != 0 &&
		    strncmp(key, "tag-", 4) != 0 &&
		    strncmp(key, "title-", 6) != 0) {
			g_autofree gchar *plugin_name = NULL;

			plugin_name = g_strndup(key,
			                        strlen(key) - strlen("-color"));
			bar_forward_setting(self, bar, plugin_name, "color",
			                   (const gchar *)v);
			continue;
		}

		if (strncmp(key, "tag-", 4) == 0) {
			bar_forward_setting(self, bar, "tags", key + 4,
			                   (const gchar *)v);
			continue;
		}
		if (strncmp(key, "title-", 6) == 0) {
			bar_forward_setting(self, bar, "title", key + 6,
			                   (const gchar *)v);
			continue;
		}
		if (strncmp(key, "widget-data-", 12) == 0) {
			g_hash_table_insert(self->widget_data,
			                    g_strdup(key + 12),
			                    g_strdup((const gchar *)v));
			continue;
		}
	}

	val = g_hash_table_lookup(settings, "show-tags");
	if (val != NULL)
		bar_forward_setting(self, bar, "tags", "visible", val);

	val = g_hash_table_lookup(settings, "title");
	if (val != NULL)
		bar_forward_setting(self, bar, "title", "text", val);

	val = g_hash_table_lookup(settings, "cmd-interval");
	if (val != NULL)
		bar_forward_setting(self, bar, "cmd", "interval", val);
}

/*
 * The module owns two slots but a YAML `modules:' block names one
 * module, so the bottom bar is addressed by prefixing its keys with
 * `bottom-'.  A configuration pushed from Elisp or from config.c uses
 * `position: bottom' instead and arrives here as its own call; both
 * routes end in bar_configure_slot.
 */
static GHashTable *
bar_extract_prefixed(GHashTable *settings, const gchar *prefix)
{
	GHashTable *out;
	GHashTableIter iter;
	gpointer k, v;
	gsize prefix_len;

	out = NULL;
	prefix_len = strlen(prefix);

	g_hash_table_iter_init(&iter, settings);
	while (g_hash_table_iter_next(&iter, &k, &v)) {
		const gchar *key = (const gchar *)k;

		if (strncmp(key, prefix, prefix_len) != 0)
			continue;
		if (key[prefix_len] == '\0')
			continue;
		if (out == NULL) {
			out = g_hash_table_new_full(g_str_hash, g_str_equal,
			                            g_free, g_free);
		}
		g_hash_table_insert(out, g_strdup(key + prefix_len),
		                    g_strdup((const gchar *)v));
	}
	return out;
}

static void
bar_configure(GowlModule *mod, gpointer config)
{
	GowlModuleBar *self = GOWL_MODULE_BAR(mod);
	GHashTable *settings;
	g_autoptr(GHashTable) bottom = NULL;
	GowlBarPosition pos;
	const gchar *val;

	if (config == NULL)
		return;
	settings = (GHashTable *)config;

	val = g_hash_table_lookup(settings, "position");
	pos = (val != NULL && g_ascii_strcasecmp(val, "bottom") == 0)
		? GOWL_BAR_POSITION_BOTTOM : GOWL_BAR_POSITION_TOP;

	bar_configure_slot(self, &self->bars[pos], settings);

	bottom = bar_extract_prefixed(settings, "bottom-");
	if (bottom != NULL) {
		bar_configure_slot(self,
			&self->bars[GOWL_BAR_POSITION_BOTTOM], bottom);
	}

	if (self->tick_timer != NULL)
		wl_event_source_timer_update(self->tick_timer,
		                             bar_tick_ms(self));

	if (self->compositor != NULL) {
		GList *monitors, *l;

		monitors = gowl_compositor_get_monitors(
			GOWL_COMPOSITOR(self->compositor));
		for (l = monitors; l != NULL; l = l->next)
			gowl_compositor_arrangelayers(
				GOWL_COMPOSITOR(self->compositor),
				GOWL_MONITOR(l->data));
		bar_redraw_all(self);
	}
}

/**
 * bar_widget_data:
 * @self: the module
 * @key: a data key
 *
 * Returns: (transfer none) (nullable): a value pushed in by the editor
 */
const gchar *
bar_module_widget_data(GowlModuleBar *self, const gchar *key)
{
	if (self == NULL || self->widget_data == NULL || key == NULL)
		return NULL;
	return g_hash_table_lookup(self->widget_data, key);
}

/* ----------------------------------------------------------------
 * Tick
 * ---------------------------------------------------------------- */

static gint
bar_tick_ms(GowlModuleBar *self)
{
	gint min_s = 5;
	gint bi;
	guint i;

	for (bi = 0; bi < GOWL_BAR_POSITION_COUNT; bi++) {
		GowlBarInstance *bar = &self->bars[bi];

		if (!bar->enabled)
			continue;
		for (i = 0; i < bar->items->len; i++) {
			BarItem *item = g_ptr_array_index(bar->items, i);
			gint interval;

			interval = gowl_bar_plugin_get_interval(item->plugin);
			if (interval > 0 && interval < min_s)
				min_s = interval;
		}
	}

	/* A live toast has to expire on time, and a one-second floor for
	   as long as one is showing costs nothing at idle. */
	if (gowl_bar_toast_stack_size(self->toasts) > 0)
		min_s = 1;

	if (min_s < 1)
		min_s = 1;
	return min_s * 1000;
}

static int
bar_tick(void *data)
{
	GowlModuleBar *self = GOWL_MODULE_BAR(data);

	bar_poll_items(self);

	if (gowl_bar_toast_stack_expire(self->toasts, g_get_monotonic_time()))
		g_atomic_int_set(&self->redraw_pending, 1);

	if (self->panel.dirty_signature != NULL) {
		g_clear_pointer(&self->panel.dirty_signature, g_free);
		bar_panel_rebuild(self);
	}

	bar_redraw_all(self);
	bar_toast_render(self);
	g_atomic_int_set(&self->redraw_pending, 0);

	if (self->tick_timer != NULL)
		wl_event_source_timer_update(self->tick_timer,
		                             bar_tick_ms(self));
	return 0;
}

/* ----------------------------------------------------------------
 * Input
 * ---------------------------------------------------------------- */

/* Which bar slot, if any, owns monitor-local y. */
static GowlBarInstance *
bar_slot_at(GowlModuleBar *self, gint y, gint mon_h, gint *out_local_y)
{
	GowlBarInstance *top = &self->bars[GOWL_BAR_POSITION_TOP];
	GowlBarInstance *bot = &self->bars[GOWL_BAR_POSITION_BOTTOM];

	if (top->enabled && top->visible && top->bar_height > 0 &&
	    y >= 0 && y < top->bar_height) {
		if (out_local_y != NULL)
			*out_local_y = y;
		return top;
	}
	if (bot->enabled && bot->visible && bot->bar_height > 0 &&
	    y >= mon_h - bot->bar_height && y < mon_h) {
		if (out_local_y != NULL)
			*out_local_y = y - (mon_h - bot->bar_height);
		return bot;
	}
	return NULL;
}

static BarItem *
bar_item_at(GowlBarInstance *bar, gint x, gint *out_local_x)
{
	guint i;

	for (i = 0; i < bar->items->len; i++) {
		BarItem *item = g_ptr_array_index(bar->items, i);

		if (!item->slot.visible || item->slot.width <= 0)
			continue;
		if (x >= item->slot.x && x < item->slot.x + item->slot.width) {
			if (out_local_x != NULL)
				*out_local_x = x - item->slot.x;
			return item;
		}
	}
	return NULL;
}

/* Set a slider's value from a pointer x inside its track. */
static void
bar_panel_drag_to(GowlModuleBar *self, gint px)
{
	GowlBarPanelItem *item;
	gdouble fraction;

	if (!self->panel.drag_active || self->panel.drag_id == NULL ||
	    self->panel.drag_w <= 0)
		return;

	fraction = (gdouble)(px - self->panel.drag_x) /
	           (gdouble)self->panel.drag_w;
	if (fraction < 0.0)
		fraction = 0.0;
	if (fraction > 1.0)
		fraction = 1.0;

	item = gowl_bar_panel_find(self->panel.panel, self->panel.drag_id);
	if (item != NULL) {
		gdouble step = gowl_bar_panel_item_get_step(item);

		if (step > 0.0)
			fraction = step * (gdouble)((gint)(fraction / step +
			                                   0.5));
		/* Move the model before telling the plugin: the drag has
		   to track the pointer even when the plugin's own state
		   only catches up asynchronously (a volume set is a
		   subprocess). */
		gowl_bar_panel_item_set_fraction(item, fraction);
	}

	bar_panel_deliver(self, self->panel.drag_id, -1, fraction, BTN_LEFT);
	bar_panel_render(self);
}

/* A plugin's click callback, packaged so it can run under the fault
   guard --- which takes a single opaque pointer. */
typedef struct {
	GowlBarPlugin *plugin;
	guint          button;
	gint           x;
	gint           y;
	guint          modifiers;
	gboolean       consumed;
} BarClickCtx;

static void
bar_click_body(gpointer data)
{
	BarClickCtx *ctx = data;

	ctx->consumed = gowl_bar_plugin_on_click(ctx->plugin, ctx->button,
	                                         ctx->x, ctx->y,
	                                         ctx->modifiers);
}

static gboolean
bar_handle_button(GowlBarProvider *provider, gpointer monitor, gint x, gint y,
                  guint button, gboolean pressed, guint modifiers)
{
	GowlModuleBar *self = GOWL_MODULE_BAR(provider);
	GowlBarInstance *bar;
	BarItem *item;
	gint mon_x, mon_y, mon_w, mon_h;
	gint local_y, local_x;

	if (monitor == NULL)
		return FALSE;

	if (!pressed) {
		/* Claim a release only when the bar has something in
		   flight.  Claiming every release that happens to land over
		   the bar would swallow the one ending a drag that started
		   inside a window -- the client would sit believing its
		   button was still held. */
		if (self->panel.drag_active) {
			self->panel.drag_active = FALSE;
			g_clear_pointer(&self->panel.drag_id, g_free);
			return TRUE;
		}
		return FALSE;
	}

	gowl_monitor_get_geometry(GOWL_MONITOR(monitor), &mon_x, &mon_y,
	                          &mon_w, &mon_h);

	/* Toasts first: they float over everything, including the bar. */
	if (self->toast_layer.monitor == monitor &&
	    bar_toast_click(self, x, y))
		return TRUE;

	/* Then an open panel. */
	if (self->panel.item != NULL) {
		gint px, py;

		if (self->panel.monitor == monitor &&
		    bar_panel_point(self, x, y, &px, &py)) {
			gint hit;

			hit = gowl_bar_hit_find(self->panel.hits, px, py);
			if (hit >= 0) {
				GowlBarHitRect *rect;

				rect = &g_array_index(self->panel.hits,
				                      GowlBarHitRect, hit);
				if (rect->kind == GOWL_BAR_ITEM_SLIDER &&
				    button == BTN_LEFT) {
					self->panel.drag_active = TRUE;
					g_free(self->panel.drag_id);
					self->panel.drag_id =
						g_strdup(rect->id);
					self->panel.drag_x = rect->x;
					self->panel.drag_w = rect->width;
					bar_panel_drag_to(self, px);
					return TRUE;
				}
				bar_panel_deliver(self, rect->id,
				                  rect->child_index, 0.0,
				                  button);
			}
			return TRUE;
		}

		/* A press anywhere else dismisses the panel and is
		   swallowed, so the click that closed a dropdown does not
		   also land in the application behind it. */
		bar_panel_close(self);
		return TRUE;
	}

	bar = bar_slot_at(self, y, mon_h, &local_y);
	if (bar == NULL)
		return FALSE;

	item = bar_item_at(bar, x, &local_x);
	if (item == NULL) {
		/* Empty bar space: consumed so a stray click does not
		   punch through to the window behind the bar. */
		return TRUE;
	}

	{
		BarClickCtx ctx;
		gint signo = 0;

		ctx.plugin    = item->plugin;
		ctx.button    = button;
		ctx.x         = local_x;
		ctx.y         = local_y;
		ctx.modifiers = modifiers;
		ctx.consumed  = FALSE;

		/* Guarded like everything else a plugin exposes: a null
		   dereference in somebody's button handler must not be a
		   logout. */
		if (!gowl_bar_guard_call(bar_click_body, &ctx, &signo,
		                         gowl_bar_plugin_get_id(item->plugin))) {
			const gchar *name;

			name = gowl_bar_plugin_get_setting(item->plugin,
			                                   "name");
			gowl_bar_registry_quarantine(self->registry,
				(name != NULL) ? name
				               : gowl_bar_plugin_get_id(
						item->plugin),
				"it faulted handling a click");
			return TRUE;
		}

		if (ctx.consumed) {
			bar_redraw_all(self);
			return TRUE;
		}
	}

	if (button == BTN_LEFT && gowl_bar_plugin_has_panel(item->plugin)) {
		bar_panel_open(self, bar, item, monitor);
		return TRUE;
	}

	/* Not consumed and no panel: let the compositor's own bar
	   handling (the tag row) have it. */
	return FALSE;
}

static gboolean
bar_handle_motion(GowlBarProvider *provider, gpointer monitor, gint x, gint y)
{
	GowlModuleBar *self = GOWL_MODULE_BAR(provider);
	GowlBarInstance *bar;
	gint mon_x, mon_y, mon_w, mon_h;
	gint px, py;

	if (monitor == NULL)
		return FALSE;

	gowl_monitor_get_geometry(GOWL_MONITOR(monitor), &mon_x, &mon_y,
	                          &mon_w, &mon_h);

	if (self->panel.drag_active && self->panel.monitor == monitor) {
		gint lx;

		lx = x - self->panel.surf_x - self->panel.frame_x;
		bar_panel_drag_to(self, lx);
		return TRUE;
	}

	if (self->panel.item != NULL && self->panel.monitor == monitor &&
	    bar_panel_point(self, x, y, &px, &py)) {
		gint hit, item_index, child_index;

		hit = gowl_bar_hit_find(self->panel.hits, px, py);
		item_index  = -1;
		child_index = -1;
		if (hit >= 0) {
			GowlBarHitRect *rect;

			rect = &g_array_index(self->panel.hits,
			                      GowlBarHitRect, hit);
			item_index  = rect->item_index;
			child_index = rect->child_index;
		}
		if (item_index != self->panel.hover_item ||
		    child_index != self->panel.hover_child) {
			self->panel.hover_item  = item_index;
			self->panel.hover_child = child_index;
			bar_panel_render(self);
		}
		return TRUE;
	}

	if (self->panel.hover_item != -1 || self->panel.hover_child != -1) {
		self->panel.hover_item  = -1;
		self->panel.hover_child = -1;
		if (self->panel.item != NULL)
			bar_panel_render(self);
	}

	bar = bar_slot_at(self, y, mon_h, NULL);
	/* A bar's own pixels are claimed so the window behind never sees
	   the pointer; an open panel claims the rest of the output so a
	   drag started in it keeps arriving. */
	return (bar != NULL);
}

static gboolean
bar_handle_axis(GowlBarProvider *provider, gpointer monitor, gint x, gint y,
                gdouble delta, gint discrete, guint modifiers)
{
	GowlModuleBar *self = GOWL_MODULE_BAR(provider);
	GowlBarInstance *bar;
	BarItem *item;
	gint mon_x, mon_y, mon_w, mon_h;
	gint local_x;

	if (monitor == NULL)
		return FALSE;

	gowl_monitor_get_geometry(GOWL_MONITOR(monitor), &mon_x, &mon_y,
	                          &mon_w, &mon_h);

	if (self->panel.item != NULL && self->panel.monitor == monitor &&
	    bar_panel_point(self, x, y, NULL, NULL)) {
		gint step;

		step = gowl_bar_theme_metric(self->panel.bar->theme,
		                             GOWL_BAR_METRIC_ROW_HEIGHT);
		self->panel.scroll += (discrete != 0)
			? discrete * step
			: (gint)(delta * (gdouble)step / 10.0);
		if (self->panel.scroll < 0)
			self->panel.scroll = 0;
		bar_panel_render(self);
		return TRUE;
	}

	bar = bar_slot_at(self, y, mon_h, NULL);
	if (bar == NULL)
		return FALSE;

	item = bar_item_at(bar, x, &local_x);
	if (item == NULL)
		return FALSE;

	if (gowl_bar_plugin_on_scroll(item->plugin, delta, discrete,
	                              modifiers)) {
		bar_redraw_all(self);
		return TRUE;
	}
	return FALSE;
}

/* ----------------------------------------------------------------
 * Keyboard, while a panel is open
 * ---------------------------------------------------------------- */

/* Move the keyboard cursor to the next interactive row. */
static void
bar_panel_focus_step(GowlModuleBar *self, gint direction)
{
	gint n, start, i;

	if (self->panel.hits == NULL || self->panel.hits->len == 0)
		return;

	n = (gint)self->panel.hits->len;
	start = -1;
	for (i = 0; i < n; i++) {
		GowlBarHitRect *r;

		r = &g_array_index(self->panel.hits, GowlBarHitRect, i);
		if (r->item_index == self->panel.focus_item &&
		    r->child_index == self->panel.focus_child) {
			start = i;
			break;
		}
	}

	i = (start < 0) ? (direction > 0 ? 0 : n - 1) : start + direction;
	if (i < 0)
		i = n - 1;
	if (i >= n)
		i = 0;

	{
		GowlBarHitRect *r;

		r = &g_array_index(self->panel.hits, GowlBarHitRect, i);
		self->panel.focus_item  = r->item_index;
		self->panel.focus_child = r->child_index;

		/* Keep the cursor on screen: a keyboard walk that runs off
		   the bottom of a scrolling panel is a walk into nothing. */
		if (r->y < 0)
			self->panel.scroll += r->y - 8;
		else if (r->y + r->height > self->panel.frame_h)
			self->panel.scroll +=
				(r->y + r->height) - self->panel.frame_h + 8;
		if (self->panel.scroll < 0)
			self->panel.scroll = 0;
	}
	bar_panel_render(self);
}

static void
bar_panel_activate_focus(GowlModuleBar *self)
{
	gint i;

	if (self->panel.hits == NULL || self->panel.focus_item < 0)
		return;

	for (i = 0; i < (gint)self->panel.hits->len; i++) {
		GowlBarHitRect *r;

		r = &g_array_index(self->panel.hits, GowlBarHitRect, i);
		if (r->item_index == self->panel.focus_item &&
		    r->child_index == self->panel.focus_child) {
			bar_panel_deliver(self, r->id, r->child_index, 0.0,
			                  BTN_LEFT);
			return;
		}
	}
}

static gboolean
bar_handle_key(GowlKeybindHandler *handler, guint modifiers, guint keysym,
               gboolean pressed)
{
	GowlModuleBar *self = GOWL_MODULE_BAR(handler);

	(void)modifiers;

	if (self->panel.item == NULL)
		return FALSE;

	switch (keysym) {
	case XKB_KEY_Escape:
		if (pressed)
			bar_panel_close(self);
		return TRUE;
	case XKB_KEY_Down:
	case XKB_KEY_j:
	case XKB_KEY_Tab:
		if (pressed)
			bar_panel_focus_step(self, 1);
		return TRUE;
	case XKB_KEY_Up:
	case XKB_KEY_k:
	case XKB_KEY_ISO_Left_Tab:
		if (pressed)
			bar_panel_focus_step(self, -1);
		return TRUE;
	case XKB_KEY_Return:
	case XKB_KEY_KP_Enter:
	case XKB_KEY_space:
		if (pressed)
			bar_panel_activate_focus(self);
		return TRUE;
	default:
		break;
	}

	/*
	 * Anything else closes the panel and is NOT claimed.
	 *
	 * Swallowing every key while a dropdown is open would eat the
	 * window-manager bindings that other modules provide -- alt-tab,
	 * the overview, the cube -- and those are exactly the keys
	 * somebody reaches for to get out of a panel they opened by
	 * accident.  Closing and letting the key through does what they
	 * meant.
	 */
	if (pressed)
		bar_panel_close(self);
	return FALSE;
}

/* ----------------------------------------------------------------
 * GowlBarProvider
 * ---------------------------------------------------------------- */

static gint
bar_get_bar_height(GowlBarProvider *provider, gpointer monitor)
{
	GowlModuleBar *self = GOWL_MODULE_BAR(provider);
	GowlBarInstance *top = &self->bars[GOWL_BAR_POSITION_TOP];

	(void)monitor;
	if (top->enabled && top->visible)
		return top->bar_height;
	return 0;
}

static void
bar_get_bar_insets_impl(GowlBarProvider *provider, gpointer monitor,
                        gint *top_out, gint *bottom_out)
{
	GowlModuleBar *self = GOWL_MODULE_BAR(provider);
	GowlBarInstance *top = &self->bars[GOWL_BAR_POSITION_TOP];
	GowlBarInstance *bot = &self->bars[GOWL_BAR_POSITION_BOTTOM];
	gint t = 0, b = 0;

	(void)monitor;

	if (top->enabled && top->visible && top->bar_height > 0)
		t = top->bar_height;
	if (bot->enabled && bot->visible && bot->bar_height > 0)
		b = bot->bar_height;

	if (top_out != NULL)
		*top_out = t;
	if (bottom_out != NULL)
		*bottom_out = b;
}

static void
bar_render_bar(GowlBarProvider *provider, gpointer monitor)
{
	(void)monitor;
	bar_redraw_all(GOWL_MODULE_BAR(provider));
}

/* Tag hit-testing now comes from wherever the tags widget was laid
   out, rather than assuming it is pinned to the left edge --- with
   three regions it may be anywhere. */
static gint
bar_tag_at(GowlBarProvider *provider, gpointer monitor, gint x, gint y)
{
	GowlModuleBar *self = GOWL_MODULE_BAR(provider);
	GowlBarInstance *bar;
	gint mon_x, mon_y, mon_w, mon_h;
	gint local_y;
	guint i;

	if (monitor == NULL)
		return -1;

	gowl_monitor_get_geometry(GOWL_MONITOR(monitor), &mon_x, &mon_y,
	                          &mon_w, &mon_h);
	bar = bar_slot_at(self, y, mon_h, &local_y);
	if (bar == NULL)
		return -1;

	for (i = 0; i < bar->items->len; i++) {
		BarItem *item = g_ptr_array_index(bar->items, i);
		const gchar *name;
		gint tag_count, box_w, index;

		name = gowl_bar_plugin_get_setting(item->plugin, "name");
		if (g_strcmp0(name, "tags") != 0)
			continue;
		if (!item->slot.visible || item->slot.width <= 0)
			continue;
		if (x < item->slot.x || x >= item->slot.x + item->slot.width)
			continue;

		tag_count = gowl_bar_plugin_get_setting_int(item->plugin,
		                                            "tag-count", 9);
		if (tag_count <= 0)
			return -1;
		box_w = item->slot.width / tag_count;
		if (box_w <= 0)
			return -1;
		index = (x - item->slot.x) / box_w;
		if (index < 0 || index >= tag_count)
			return -1;
		return index;
	}
	return -1;
}

static void
bar_provider_iface_init(GowlBarProviderInterface *iface)
{
	iface->get_bar_height = bar_get_bar_height;
	iface->get_bar_insets = bar_get_bar_insets_impl;
	iface->render_bar     = bar_render_bar;
	iface->tag_at         = bar_tag_at;
	iface->handle_button  = bar_handle_button;
	iface->handle_motion  = bar_handle_motion;
	iface->handle_axis    = bar_handle_axis;
}

static void
bar_keybind_iface_init(GowlKeybindHandlerInterface *iface)
{
	iface->handle_key = bar_handle_key;
}

/* ----------------------------------------------------------------
 * IPC
 * ---------------------------------------------------------------- */

static gchar *
bar_handle_command(GowlIpcHandler *handler, const gchar *command,
                   const gchar *args)
{
	GowlModuleBar *self = GOWL_MODULE_BAR(handler);
	g_autoptr(GError) error = NULL;

	if (command == NULL)
		return NULL;

	if (strcmp(command, "bar-plugins") == 0) {
		g_auto(GStrv) names = NULL;
		GString *out;
		gint i;

		names = gowl_bar_registry_list(self->registry);
		out = g_string_new(NULL);
		for (i = 0; names != NULL && names[i] != NULL; i++) {
			g_autofree gchar *line = NULL;

			line = gowl_bar_registry_describe(self->registry,
			                                  names[i]);
			if (line != NULL)
				g_string_append_printf(out, "%s\n", line);
		}
		return g_string_free(out, FALSE);
	}

	if (strcmp(command, "bar-widgets") == 0) {
		GString *out;
		gint bi;
		guint i;

		/* The laid-out truth, per slot and region.  "Why is my
		   widget missing" has three plausible answers -- it is not
		   registered, it measured to nothing, or the region it went
		   into is not the one that was configured -- and this is
		   the only thing that distinguishes them. */
		out = g_string_new(NULL);
		for (bi = 0; bi < GOWL_BAR_POSITION_COUNT; bi++) {
			GowlBarInstance *bar = &self->bars[bi];

			g_string_append_printf(out, "%s\t%s\t%s\tanchor=%s\n",
				(bi == GOWL_BAR_POSITION_TOP) ? "top" : "bottom",
				bar->enabled ? "enabled" : "disabled",
				bar->visible ? "visible" : "hidden",
				(bar->anchor_id != NULL) ? bar->anchor_id
				                         : "-");

			for (i = 0; i < bar->items->len; i++) {
				BarItem *item;
				g_autofree gchar *label = NULL;

				const gchar *mark;

				item = g_ptr_array_index(bar->items, i);
				label = gowl_bar_plugin_dup_label(item->plugin);

				/* Two different problems wear the same
				   symptom.  `[off]' is the plugin saying it
				   has nothing to show here -- no battery, no
				   tailnet.  `[hidden]' is the layout saying
				   it did not fit. */
				if (!gowl_bar_plugin_get_visible(item->plugin))
					mark = "[off] ";
				else if (!item->slot.visible)
					mark = "[hidden] ";
				else
					mark = "";

				g_string_append_printf(out,
					"  %-6s %-28s %s%s\n",
					gowl_bar_layout_region_name(item->region),
					item->spec, mark,
					(label != NULL) ? label : "");
			}
		}
		return g_string_free(out, FALSE);
	}

	if (strcmp(command, "bar-plugin-load") == 0) {
		g_autofree gchar *resolved = NULL;

		if (args == NULL || args[0] == '\0')
			return g_strdup("error: a name or path is required\n");
		/* A bare name is searched for, so this matches what the
		   configuration accepts. */
		resolved = gowl_bar_registry_resolve_file(self->registry,
		                                          args, &error);
		if (resolved == NULL)
			return g_strdup_printf("error: %s\n", error->message);
		if (!gowl_bar_registry_load_file(self->registry, resolved,
		                                 &error))
			return g_strdup_printf("error: %s\n", error->message);
		return g_strdup_printf("loaded %s\n", resolved);
	}

	if (strcmp(command, "bar-plugin-unload") == 0) {
		if (args == NULL || args[0] == '\0')
			return g_strdup("error: a plugin name is required\n");
		if (!gowl_bar_registry_unload(self->registry, args, &error))
			return g_strdup_printf("error: %s\n", error->message);
		bar_redraw_all(self);
		return g_strdup_printf("unloaded %s\n", args);
	}

	if (strcmp(command, "bar-plugin-reload") == 0) {
		if (args == NULL || args[0] == '\0')
			return g_strdup("error: a plugin name is required\n");
		if (!gowl_bar_registry_reload(self->registry, args, &error))
			return g_strdup_printf("error: %s\n", error->message);
		bar_redraw_all(self);
		return g_strdup_printf("reloaded %s\n", args);
	}

	if (strcmp(command, "bar-plugin-clear") == 0) {
		if (args == NULL || args[0] == '\0')
			return g_strdup("error: a plugin name is required\n");
		if (!gowl_bar_registry_clear_quarantine(self->registry, args))
			return g_strdup_printf("'%s' was not quarantined\n",
			                       args);
		return g_strdup_printf("cleared %s\n", args);
	}

	if (strcmp(command, "bar-quarantined") == 0) {
		g_auto(GStrv) names = NULL;
		GString *out;
		gint i;

		names = gowl_bar_registry_list_quarantined(self->registry);
		out = g_string_new(NULL);
		for (i = 0; names != NULL && names[i] != NULL; i++) {
			g_string_append_printf(out, "%s\t%s\n", names[i],
				gowl_bar_registry_quarantine_reason(
					self->registry, names[i]));
		}
		if (out->len == 0)
			g_string_append(out, "nothing is quarantined\n");
		return g_string_free(out, FALSE);
	}

	if (strcmp(command, "bar-panel") == 0) {
		if (args == NULL || args[0] == '\0') {
			bar_panel_close(self);
			return g_strdup("closed\n");
		}
		gowl_bar_host_open_panel(GOWL_BAR_HOST(self), args);
		return g_strdup_printf("opened %s\n", args);
	}

	if (strcmp(command, "bar-notify") == 0) {
		GowlBarToast *toast;
		g_auto(GStrv) parts = NULL;

		if (args == NULL || args[0] == '\0')
			return g_strdup("error: a summary is required\n");

		/* `summary|body|panel-id' -- enough for a shell one-liner
		   to raise a toast that opens a dropdown. */
		parts = g_strsplit(args, "|", 3);
		toast = gowl_bar_toast_new(parts[0],
			(parts[1] != NULL && parts[1][0] != '\0')
				? parts[1] : NULL);
		if (parts[1] != NULL && parts[2] != NULL &&
		    parts[2][0] != '\0') {
			gowl_bar_toast_set_panel(toast, parts[2]);
			gowl_bar_toast_set_hint(toast, "Click to open");
		}
		gowl_bar_toast_stack_push(self->toasts, toast);
		bar_toast_render(self);
		return g_strdup("ok\n");
	}

	if (strcmp(command, "bar-dismiss") == 0) {
		gowl_bar_toast_stack_clear(self->toasts);
		bar_toast_render(self);
		return g_strdup("ok\n");
	}

	return NULL;
}

static void
bar_ipc_iface_init(GowlIpcHandlerInterface *iface)
{
	iface->handle_command = bar_handle_command;
}

/* ----------------------------------------------------------------
 * Module lifecycle
 * ---------------------------------------------------------------- */

static gboolean
bar_activate(GowlModule *mod)
{
	(void)mod;
	return TRUE;
}

static void
bar_disconnect_signals(GowlModuleBar *self)
{
	if (self->compositor == NULL)
		return;

	if (self->focus_handler_id != 0) {
		g_signal_handler_disconnect(self->compositor,
		                            self->focus_handler_id);
		self->focus_handler_id = 0;
	}
	if (self->client_added_id != 0) {
		g_signal_handler_disconnect(self->compositor,
		                            self->client_added_id);
		self->client_added_id = 0;
	}
	if (self->client_removed_id != 0) {
		g_signal_handler_disconnect(self->compositor,
		                            self->client_removed_id);
		self->client_removed_id = 0;
	}
}

static void
bar_deactivate(GowlModule *mod)
{
	GowlModuleBar *self = GOWL_MODULE_BAR(mod);

	bar_disconnect_signals(self);

	if (self->tick_timer != NULL) {
		wl_event_source_remove(self->tick_timer);
		self->tick_timer = NULL;
	}

	bar_panel_clear_state(self);
	bar_toast_destroy_surface(self);
	bar_destroy_all_surfaces(self);
	self->compositor = NULL;
}

static const gchar *
bar_get_name(GowlModule *mod)
{
	(void)mod;
	return "bar";
}

static const gchar *
bar_get_description(GowlModule *mod)
{
	(void)mod;
	return "Status bar with pluggable widgets, dropdown panels and toasts";
}

static const gchar *
bar_get_version(GowlModule *mod)
{
	(void)mod;
	return "0.3.0";
}

static void
bar_on_focus_changed(GowlCompositor *comp, GObject *client, gpointer user_data)
{
	GowlModuleBar *self = GOWL_MODULE_BAR(user_data);

	(void)comp;
	(void)client;
	g_atomic_int_set(&self->redraw_pending, 1);
	bar_redraw_all(self);
}

static void
bar_on_client_changed(GowlCompositor *comp, GObject *client,
                      gpointer user_data)
{
	GowlModuleBar *self = GOWL_MODULE_BAR(user_data);

	(void)comp;
	(void)client;
	bar_redraw_all(self);
}

/* Rebuild both slots' themes from the session palette, keeping any
   role the config pinned. */
static void
bar_apply_palette(GowlModuleBar *self)
{
	GowlConfig *config;
	GowlPalette *palette;
	gint i;

	if (self->compositor == NULL)
		return;
	config = gowl_compositor_get_config(GOWL_COMPOSITOR(self->compositor));
	if (config == NULL)
		return;
	palette = gowl_config_get_palette(config);
	if (palette == NULL)
		return;

	for (i = 0; i < GOWL_BAR_POSITION_COUNT; i++)
		gowl_bar_theme_apply_palette(self->bars[i].theme, palette);
}

static void
bar_on_startup(GowlStartupHandler *handler, gpointer compositor)
{
	GowlModuleBar *self = GOWL_MODULE_BAR(handler);
	GowlCompositor *comp = GOWL_COMPOSITOR(compositor);
	GList *monitors, *l;
	struct wl_event_loop *loop;
	g_autofree gchar *culprit = NULL;

	self->compositor     = compositor;
	self->env.compositor = compositor;
	bar_env_set(&self->env);

	bar_apply_palette(self);

	/* Recover before anything is loaded: a plugin that took the last
	   session down must be held back before it gets a second try. */
	culprit = gowl_bar_registry_recover_journal(self->registry);

	bar_scan_plugin_dirs(self);

	self->focus_handler_id = g_signal_connect(compositor, "focus-changed",
		G_CALLBACK(bar_on_focus_changed), self);
	self->client_added_id = g_signal_connect(compositor, "client-added",
		G_CALLBACK(bar_on_client_changed), self);
	self->client_removed_id = g_signal_connect(compositor, "client-removed",
		G_CALLBACK(bar_on_client_changed), self);

	monitors = gowl_compositor_get_monitors(comp);
	for (l = monitors; l != NULL; l = l->next) {
		GowlMonitor *mon = GOWL_MONITOR(l->data);
		gint bi;

		if (!gowl_monitor_get_enabled(mon))
			continue;
		for (bi = 0; bi < GOWL_BAR_POSITION_COUNT; bi++)
			bar_create_surface(self, &self->bars[bi], mon);
	}

	loop = wl_display_get_event_loop(gowl_compositor_get_wl_display(comp));
	if (loop != NULL) {
		self->tick_timer = wl_event_loop_add_timer(loop, bar_tick, self);
		if (self->tick_timer != NULL)
			wl_event_source_timer_update(self->tick_timer,
			                             bar_tick_ms(self));
	}

	monitors = gowl_compositor_get_monitors(comp);
	for (l = monitors; l != NULL; l = l->next)
		gowl_compositor_arrangelayers(comp, GOWL_MONITOR(l->data));
}

static void
bar_startup_iface_init(GowlStartupHandlerInterface *iface)
{
	iface->on_startup = bar_on_startup;
}

static void
bar_on_shutdown(GowlShutdownHandler *handler, gpointer compositor)
{
	GowlModuleBar *self = GOWL_MODULE_BAR(handler);

	(void)compositor;

	bar_disconnect_signals(self);

	if (self->tick_timer != NULL) {
		wl_event_source_remove(self->tick_timer);
		self->tick_timer = NULL;
	}

	bar_panel_clear_state(self);
	bar_toast_destroy_surface(self);
	bar_destroy_all_surfaces(self);
	bar_env_set(NULL);
	self->compositor     = NULL;
	self->env.compositor = NULL;
}

static void
bar_shutdown_iface_init(GowlShutdownHandlerInterface *iface)
{
	iface->on_shutdown = bar_on_shutdown;
}

/* ----------------------------------------------------------------
 * GObject
 * ---------------------------------------------------------------- */

static void
gowl_module_bar_finalize(GObject *object)
{
	GowlModuleBar *self = GOWL_MODULE_BAR(object);
	gint i;

	/* Drain the pool first: a worker holds a reference to a plugin
	   and will touch it after this returns otherwise. */
	if (self->worker_pool != NULL) {
		g_thread_pool_free(self->worker_pool, FALSE, TRUE);
		self->worker_pool = NULL;
	}

	bar_panel_clear_state(self);
	bar_toast_destroy_surface(self);
	g_clear_pointer(&self->toast_layer.hits, g_array_unref);

	for (i = 0; i < GOWL_BAR_POSITION_COUNT; i++) {
		GowlBarInstance *bar = &self->bars[i];

		g_clear_pointer(&bar->items, g_ptr_array_unref);
		g_clear_pointer(&bar->surfaces, g_hash_table_unref);
		g_clear_pointer(&bar->theme, gowl_bar_theme_free);
		g_free(bar->anchor_id);
	}

	g_clear_object(&self->registry);
	g_clear_object(&self->toasts);
	g_clear_pointer(&self->sysinfo, bar_sysinfo_free);
	g_clear_pointer(&self->widget_data, g_hash_table_unref);
	g_free(self->state_dir);
	g_free(self->plugin_dir);
	g_clear_pointer(&self->plugin_dirs, g_strfreev);
	g_clear_pointer(&self->scanned_dirs, g_hash_table_unref);

	g_clear_object(&self->measure_layout);
	if (self->measure_cr != NULL) {
		cairo_destroy(self->measure_cr);
		self->measure_cr = NULL;
	}
	if (self->measure_surface != NULL) {
		cairo_surface_destroy(self->measure_surface);
		self->measure_surface = NULL;
	}

	bar_env_set(NULL);

	G_OBJECT_CLASS(gowl_module_bar_parent_class)->finalize(object);
}

static void
gowl_module_bar_class_init(GowlModuleBarClass *klass)
{
	GObjectClass *obj_class = G_OBJECT_CLASS(klass);
	GowlModuleClass *mod_class = GOWL_MODULE_CLASS(klass);

	obj_class->finalize = gowl_module_bar_finalize;

	mod_class->activate        = bar_activate;
	mod_class->deactivate      = bar_deactivate;
	mod_class->get_name        = bar_get_name;
	mod_class->get_description = bar_get_description;
	mod_class->get_version     = bar_get_version;
	mod_class->configure       = bar_configure;
}

static void
bar_instance_init(GowlBarInstance *bar, GowlBarPosition position)
{
	memset(bar, 0, sizeof(*bar));

	bar->position   = position;
	bar->enabled    = FALSE;
	bar->visible    = TRUE;
	bar->bar_height = 30;
	bar->anchor     = -1;
	bar->defaults_pending = FALSE;
	bar->theme      = gowl_bar_theme_new();
	bar->items      = g_ptr_array_new_with_free_func(bar_item_free);
	bar->surfaces   = g_hash_table_new_full(g_str_hash, g_str_equal,
	                                        g_free, bar_surface_free);

	/* Slightly translucent by default: the bar sits over the
	   wallpaper and reads better as a layer than as a slab. */
	gowl_bar_theme_set_color(bar->theme, GOWL_BAR_COLOR_BASE, "#1e1e2ee6");
	gowl_bar_theme_set_font(bar->theme, "monospace 11");
}

/*
 * The shipped layout, as a settings table.
 *
 * Fed through bar_configure_slot() -- the same path a user
 * configuration takes -- so a default and a configured value cannot
 * diverge in how they are interpreted, and every per-widget key works
 * here exactly as it does in a config file.
 *
 * defaults_pending is put back afterwards, because the rule it drives
 * still has to hold: the FIRST configuration naming any widget list
 * replaces this layout wholesale rather than adding to it.  Without
 * that, a user who lists their own widgets gets them alongside these,
 * and the bar shows two clocks.
 *
 * ORDER.  The right region packs leftwards from the far edge, so the
 * first entry here ends up FURTHEST RIGHT.  The list below therefore
 * reads backwards from what appears on screen, which left to right is:
 * tailscale, cpu, memory, disk, network, bluetooth, battery.  The centre is the
 * other way round -- entries before the anchor sit to its left -- so it
 * reads as it appears: the screenshot, recorder and caffeine buttons,
 * then the clock dead centre, then the weather.
 *
 * Colours name palette ROLES rather than hex, so the whole bar --
 * panels, toasts and third-party widgets included -- follows the
 * session flavour, which is Catppuccin Mocha by default.
 */
static void
bar_apply_shipped_defaults(GowlModuleBar *self)
{
	static const gchar *const top_kv[] = {
		"height",            "30",

		"widgets-left",      "tags title",
		"widgets-center",    "screenshot recorder toggle:caffeine clock weather",
		/* Reversed: first entry lands furthest right. */
		"widgets-right",     "battery bluetooth network disk memory cpu tailscale",
		"center-anchor",     "clock",

		"cpu-color",         "green",
		"memory-color",      "blue",
		"disk-color",        "yellow",
		"temp-color",        "peach",
		"battery-color",     "teal",
		"clock-color",       "text",
		"weather-color",     "sky",
		"audio-color",       "mauve",
		"network-color",     "sapphire",
		"tailscale-color",   "teal",

		"clock.format",      "%a %b %d  %H:%M",
		"title.max-width",   "520",
		"network.ping-host", "1.1.1.1",

		/* The title reads as a path; splitting it and cycling the
		   palette makes the interesting end easy to find. */
		"title-delimiters",       "-._/: *",
		"title-delimiter-color",  "#585b70",
		"title-palette",
		"#89b4fa #a6e3a1 #f9e2af #f5c2e7 #94e2d5 #cba6f7 #fab387 #89dceb",

		/* Caffeine asks its state-command whether idle inhibition is
		   running and flips it, so the glyph always reflects reality
		   even when something else started it. */
		"toggle:caffeine.state-command", "pgrep -x systemd-inhibit",
		"toggle:caffeine.command-on",
		"sh -c 'systemd-inhibit --what=idle:sleep --why=caffeine "
		"sleep infinity &'",
		"toggle:caffeine.command-off",
		"pkill -f 'systemd-inhibit.*caffeine'",
		/*
		 * A Nerd Font coffee cup (U+F0F4), NOT the emoji U+2615.
		 *
		 * A colour emoji is drawn from a bitmap font with its own
		 * palette baked in, so set_color() does nothing to it: the
		 * button looked exactly the same whether caffeine was on or
		 * off, which is the entire signal the widget exists to give.
		 * Monochrome glyphs take the theme colour.
		 */
		"toggle:caffeine.icon-on",  "\xef\x83\xb4",
		"toggle:caffeine.icon-off", "\xef\x83\xb4",
		"toggle:caffeine.color-on",  "yellow",
		"toggle:caffeine.color-off", "muted",
		NULL
	};
	static const gchar *const bottom_kv[] = {
		"enabled",         "true",
		"height",          "26",

		"widgets-left",    "user host git",
		/* Reversed: reads as ip, podman, display left to right. */
		"widgets-right",   "display podman ip",

		/* The tag row belongs to the top bar; two copies is noise. */
		"tags.visible",    "false",

		"ip-color",        "mauve",
		"podman-color",    "peach",
		"git-color",       "lavender",
		NULL
	};
	static const struct {
		GowlBarPosition           pos;
		const gchar *const *const kv;
	} shipped[] = {
		{ GOWL_BAR_POSITION_TOP,    top_kv },
		{ GOWL_BAR_POSITION_BOTTOM, bottom_kv }
	};
	gsize b;

	for (b = 0; b < G_N_ELEMENTS(shipped); b++) {
		GowlBarInstance *bar = &self->bars[shipped[b].pos];
		GHashTable      *kv;
		gsize            i;

		kv = g_hash_table_new(g_str_hash, g_str_equal);
		for (i = 0; shipped[b].kv[i] != NULL; i += 2)
			g_hash_table_insert(kv,
				(gpointer)shipped[b].kv[i],
				(gpointer)shipped[b].kv[i + 1]);

		bar->enabled = TRUE;
		bar_configure_slot(self, bar, kv);
		g_hash_table_destroy(kv);

		/* Still the shipped layout, so the first real configuration
		   still replaces it rather than adding to it. */
		bar->defaults_pending = TRUE;
	}
}

/*
 * Where a plugin named without a path is looked for, most specific
 * first.
 *
 * @configured is the `plugin-dir' setting, which may name several
 * directories separated by ':' the way PATH does.  It comes first so a
 * user or a development tree can shadow anything shipped, and the
 * remaining entries are always searched, so setting it ADDS somewhere
 * to look rather than cutting off the defaults -- which is what a
 * config key like this is nearly always meant to do.
 *
 * The first entry doubles as the directory scanned wholesale at
 * startup, so dropping a file in it is enough and naming it is
 * optional.
 */
/*
 * Auto-load every plugin sitting in the search path.  Each directory is
 * remembered, so a later rebuild of the path only scans what is new: the
 * registry has no notion of "already loaded this file", and scanning a
 * directory twice would load each plugin in it twice.
 */
static void
bar_scan_plugin_dirs(GowlModuleBar *self)
{
	gint i;

	if (self->plugin_dirs == NULL)
		return;

	for (i = 0; self->plugin_dirs[i] != NULL; i++) {
		const gchar *dir = self->plugin_dirs[i];

		if (g_hash_table_contains(self->scanned_dirs, dir))
			continue;
		g_hash_table_add(self->scanned_dirs, g_strdup(dir));
		gowl_bar_registry_load_directory(self->registry, dir);
	}
}

static void
bar_rebuild_plugin_search_path(GowlModuleBar *self, const gchar *configured)
{
	g_autoptr(GPtrArray) dirs = NULL;
	const gchar *env_dir;

	dirs = g_ptr_array_new_with_free_func(g_free);

	if (configured != NULL && configured[0] != '\0') {
		g_auto(GStrv) parts = g_strsplit(configured, ":", -1);
		gint i;

		for (i = 0; parts[i] != NULL; i++) {
			if (parts[i][0] != '\0')
				g_ptr_array_add(dirs, g_strdup(parts[i]));
		}
	}

	env_dir = g_getenv("GOWL_BAR_PLUGIN_DIR");
	if (env_dir != NULL && env_dir[0] != '\0')
		g_ptr_array_add(dirs, g_strdup(env_dir));

	g_ptr_array_add(dirs, g_build_filename(g_get_user_config_dir(),
	                                       "gowl", "bar-plugins", NULL));
	g_ptr_array_add(dirs, g_build_filename(g_get_user_data_dir(),
	                                       "gowl", "bar-plugins", NULL));
	/*
	 * System locations spelled out rather than taken from
	 * GOWL_DATADIR: that macro reaches a module through a sub-make
	 * which eats the quoting, so -DGOWL_DATADIR=\"/usr/share\" arrives
	 * as a bare path and the expansion is a syntax error.  The same
	 * trap already bit G_LOG_DOMAIN here.
	 */
	g_ptr_array_add(dirs, g_strdup("/usr/local/share/gowl/bar-plugins"));
	g_ptr_array_add(dirs, g_strdup("/usr/share/gowl/bar-plugins"));
	g_ptr_array_add(dirs, NULL);

	g_free(self->plugin_dir);
	self->plugin_dir = g_strdup(g_ptr_array_index(dirs, 0));
	g_strfreev(self->plugin_dirs);
	self->plugin_dirs = g_strdupv((GStrv)dirs->pdata);
	gowl_bar_registry_set_search_path(self->registry,
		(const gchar * const *)dirs->pdata);
}

static void
gowl_module_bar_init(GowlModuleBar *self)
{
	gint i;

	gowl_bar_guard_init();

	for (i = 0; i < GOWL_BAR_POSITION_COUNT; i++)
		bar_instance_init(&self->bars[i], (GowlBarPosition)i);

	self->sysinfo    = bar_sysinfo_new();
	self->toasts     = gowl_bar_toast_stack_new();
	self->state_dir  = g_build_filename(g_get_user_state_dir(), "gowl",
	                                    NULL);
	self->registry   = gowl_bar_registry_new(self->state_dir);
	self->widget_data = g_hash_table_new_full(g_str_hash, g_str_equal,
	                                          g_free, g_free);

	self->scanned_dirs = g_hash_table_new_full(g_str_hash, g_str_equal,
	                                           g_free, NULL);
	bar_rebuild_plugin_search_path(self, NULL);

	self->env.sysinfo    = self->sysinfo;
	self->env.registry   = self->registry;
	self->env.compositor = NULL;
	bar_env_set(&self->env);

	bar_register_core_plugins(self->registry);
	bar_register_system_plugins(self->registry);
	bar_register_net_plugins(self->registry);
	bar_register_desktop_plugins(self->registry);

	g_signal_connect(self->registry, "plugin-unloaded",
	                 G_CALLBACK(bar_on_plugin_unloaded), self);
	g_signal_connect(self->registry, "plugin-failed",
	                 G_CALLBACK(bar_on_plugin_failed), self);
	g_signal_connect(self->toasts, "changed",
	                 G_CALLBACK(bar_on_toasts_changed), self);

	self->panel.hover_item  = -1;
	self->panel.hover_child = -1;
	self->panel.focus_item  = -1;
	self->panel.focus_child = -1;

	/* Four threads is plenty: the async pollers are subprocess waits,
	   not computation, and a deeper pool only means more concurrent
	   `podman ps' invocations. */
	self->worker_pool = g_thread_pool_new(bar_work_run, NULL, 4, FALSE,
	                                      NULL);

	bar_apply_shipped_defaults(self);
}

/* ----------------------------------------------------------------
 * Shared-object entry point
 * ---------------------------------------------------------------- */

G_MODULE_EXPORT GType
gowl_module_register(void)
{
	return GOWL_TYPE_MODULE_BAR;
}
