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

/**
 * GowlModuleWallpaper:
 *
 * A compositor module that loads image files via gdk-pixbuf and
 * displays them as per-monitor desktop backgrounds.  Each monitor
 * gets its own wlr_scene_buffer node placed in the BG layer of
 * the scene graph.
 *
 * Supported scaling modes:
 *   fill    - scale to cover the monitor, center-crop any excess
 *   fit     - scale to fit within the monitor, letterbox remainder
 *   center  - no scaling, center the image on the monitor
 *   stretch - scale to exactly match the monitor dimensions
 *   tile    - repeat the image across the monitor
 *
 * Configuration (YAML):
 *   modules:
 *     wallpaper:
 *       enabled: true
 *       path: "/path/to/image.png"
 *       mode: fill
 */

#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "gowl-wallpaper"

#include <glib-object.h>
#include <gmodule.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <string.h>
#include <wordexp.h>

#include <drm_fourcc.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/types/wlr_scene.h>

#include "gowl-enums.h"
#include "module/gowl-module.h"
#include "interfaces/gowl-startup-handler.h"
#include "interfaces/gowl-wallpaper-provider.h"
#include "core/gowl-compositor.h"
#include "core/gowl-monitor.h"

/* ----------------------------------------------------------------
 * Custom wlr_buffer backed by gdk-pixbuf pixel data
 * ---------------------------------------------------------------- */

/**
 * GowlPixbufBuffer:
 *
 * A wlr_buffer subtype that wraps a g_malloc'd pixel buffer in
 * DRM_FORMAT_ARGB8888.  It implements begin_data_ptr_access so
 * that wlroots can read the pixel data directly.
 */
typedef struct {
	struct wlr_buffer base;
	guchar *pixels;     /* g_malloc'd ARGB8888 pixel data */
	gsize   size;       /* total byte count */
	gint    stride;     /* bytes per row */
} GowlPixbufBuffer;

static void
pixbuf_buffer_destroy(struct wlr_buffer *buf)
{
	GowlPixbufBuffer *self;

	self = wl_container_of(buf, self, base);
	g_free(self->pixels);
	g_free(self);
}

static bool
pixbuf_buffer_begin_data_ptr_access(
	struct wlr_buffer *buf,
	uint32_t           flags,
	void             **data,
	uint32_t          *format,
	size_t            *stride
){
	GowlPixbufBuffer *self;

	(void)flags;
	self = wl_container_of(buf, self, base);
	*data   = (void *)self->pixels;
	*format = DRM_FORMAT_ARGB8888;
	*stride = (size_t)self->stride;
	return true;
}

static void
pixbuf_buffer_end_data_ptr_access(struct wlr_buffer *buf)
{
	(void)buf;
}

static const struct wlr_buffer_impl pixbuf_buffer_impl = {
	.destroy              = pixbuf_buffer_destroy,
	.begin_data_ptr_access = pixbuf_buffer_begin_data_ptr_access,
	.end_data_ptr_access  = pixbuf_buffer_end_data_ptr_access,
};

/* ----------------------------------------------------------------
 * Per-monitor wallpaper state
 * ---------------------------------------------------------------- */

/**
 * WallpaperState:
 *
 * Tracks the scene buffer node and dimensions for one monitor.
 */
typedef struct {
	struct wlr_scene_buffer *scene_buf;
	gint width;
	gint height;
} WallpaperState;

/* ----------------------------------------------------------------
 * Module type declaration
 * ---------------------------------------------------------------- */

#define GOWL_TYPE_MODULE_WALLPAPER (gowl_module_wallpaper_get_type())
G_DECLARE_FINAL_TYPE(GowlModuleWallpaper, gowl_module_wallpaper,
                     GOWL, MODULE_WALLPAPER, GowlModule)

struct _GowlModuleWallpaper {
	GowlModule  parent_instance;

	gchar      *path;         /* image file path */
	gchar      *mode;         /* scaling mode */
	GHashTable *per_monitor;  /* key: monitor name, val: WallpaperState* */
	gpointer    compositor;   /* borrowed GowlCompositor* */
};

/* Forward declarations for interface init functions */
static void wallpaper_startup_init  (GowlStartupHandlerInterface *iface);
static void wallpaper_provider_init (GowlWallpaperProviderInterface *iface);

G_DEFINE_TYPE_WITH_CODE(GowlModuleWallpaper, gowl_module_wallpaper,
	GOWL_TYPE_MODULE,
	G_IMPLEMENT_INTERFACE(GOWL_TYPE_STARTUP_HANDLER,
		wallpaper_startup_init)
	G_IMPLEMENT_INTERFACE(GOWL_TYPE_WALLPAPER_PROVIDER,
		wallpaper_provider_init))

/* ----------------------------------------------------------------
 * Pixel format conversion
 * ---------------------------------------------------------------- */

/**
 * convert_pixbuf_to_argb8888:
 * @pixbuf: source GdkPixbuf (RGB or RGBA)
 * @dst: destination buffer (must be at least dst_stride * height bytes)
 * @width: image width in pixels
 * @height: image height in pixels
 * @dst_stride: destination row stride in bytes
 *
 * Converts gdk-pixbuf pixel data (RGB or RGBA byte order) to
 * DRM_FORMAT_ARGB8888 (little-endian: B,G,R,A byte order in memory).
 */
static void
convert_pixbuf_to_argb8888(
	GdkPixbuf *pixbuf,
	guchar    *dst,
	gint       width,
	gint       height,
	gint       dst_stride
){
	const guchar *src_pixels;
	gint src_stride;
	gint n_channels;
	gboolean has_alpha;
	gint row, col;

	src_pixels = gdk_pixbuf_get_pixels(pixbuf);
	src_stride = gdk_pixbuf_get_rowstride(pixbuf);
	n_channels = gdk_pixbuf_get_n_channels(pixbuf);
	has_alpha  = gdk_pixbuf_get_has_alpha(pixbuf);

	for (row = 0; row < height; row++) {
		const guchar *src_row;
		guchar *dst_row;

		src_row = src_pixels + row * src_stride;
		dst_row = dst + row * dst_stride;

		for (col = 0; col < width; col++) {
			guchar r, g, b, a;

			r = src_row[col * n_channels + 0];
			g = src_row[col * n_channels + 1];
			b = src_row[col * n_channels + 2];
			a = has_alpha ? src_row[col * n_channels + 3] : 0xFF;

			/* DRM_FORMAT_ARGB8888 little-endian byte order: B,G,R,A */
			dst_row[col * 4 + 0] = b;
			dst_row[col * 4 + 1] = g;
			dst_row[col * 4 + 2] = r;
			dst_row[col * 4 + 3] = a;
		}
	}
}

/* ----------------------------------------------------------------
 * Image scaling
 * ---------------------------------------------------------------- */

/**
 * scale_pixbuf:
 * @source: the original image
 * @mode: scaling mode string
 * @mon_w: monitor width in pixels
 * @mon_h: monitor height in pixels
 *
 * Scales and crops the source pixbuf according to the requested
 * mode.  The returned pixbuf is always exactly mon_w x mon_h.
 *
 * Returns: (transfer full): a new GdkPixbuf sized to the monitor,
 *   or %NULL on failure
 */
static GdkPixbuf *
scale_pixbuf(
	GdkPixbuf   *source,
	const gchar *mode,
	gint         mon_w,
	gint         mon_h
){
	gint img_w, img_h;
	gdouble scale_x, scale_y, scale;

	img_w = gdk_pixbuf_get_width(source);
	img_h = gdk_pixbuf_get_height(source);

	if (g_strcmp0(mode, "stretch") == 0) {
		/* Scale to exact monitor dimensions */
		return gdk_pixbuf_scale_simple(source, mon_w, mon_h,
		                               GDK_INTERP_BILINEAR);
	}

	if (g_strcmp0(mode, "fill") == 0) {
		/* Scale to cover the monitor, then center-crop */
		GdkPixbuf *scaled;
		GdkPixbuf *cropped;
		gint scaled_w, scaled_h;
		gint crop_x, crop_y;

		scale_x = (gdouble)mon_w / (gdouble)img_w;
		scale_y = (gdouble)mon_h / (gdouble)img_h;
		scale = (scale_x > scale_y) ? scale_x : scale_y;

		scaled_w = (gint)(img_w * scale + 0.5);
		scaled_h = (gint)(img_h * scale + 0.5);

		scaled = gdk_pixbuf_scale_simple(source, scaled_w, scaled_h,
		                                 GDK_INTERP_BILINEAR);
		if (scaled == NULL)
			return NULL;

		crop_x = (scaled_w - mon_w) / 2;
		crop_y = (scaled_h - mon_h) / 2;

		cropped = gdk_pixbuf_new_subpixbuf(scaled, crop_x, crop_y,
		                                   mon_w, mon_h);
		if (cropped == NULL) {
			g_object_unref(scaled);
			return NULL;
		}

		/* subpixbuf shares pixel data with scaled, so we need a
		 * deep copy before unreffing scaled */
		{
			GdkPixbuf *result;
			result = gdk_pixbuf_copy(cropped);
			g_object_unref(cropped);
			g_object_unref(scaled);
			return result;
		}
	}

	if (g_strcmp0(mode, "fit") == 0) {
		/* Scale to fit within monitor, letterbox with black */
		GdkPixbuf *canvas;
		GdkPixbuf *scaled;
		gint scaled_w, scaled_h;
		gint offset_x, offset_y;

		scale_x = (gdouble)mon_w / (gdouble)img_w;
		scale_y = (gdouble)mon_h / (gdouble)img_h;
		scale = (scale_x < scale_y) ? scale_x : scale_y;

		scaled_w = (gint)(img_w * scale + 0.5);
		scaled_h = (gint)(img_h * scale + 0.5);

		scaled = gdk_pixbuf_scale_simple(source, scaled_w, scaled_h,
		                                 GDK_INTERP_BILINEAR);
		if (scaled == NULL)
			return NULL;

		/* Create a black canvas */
		canvas = gdk_pixbuf_new(GDK_COLORSPACE_RGB, TRUE, 8,
		                        mon_w, mon_h);
		if (canvas == NULL) {
			g_object_unref(scaled);
			return NULL;
		}
		gdk_pixbuf_fill(canvas, 0x000000FF);

		/* Center the scaled image on the canvas */
		offset_x = (mon_w - scaled_w) / 2;
		offset_y = (mon_h - scaled_h) / 2;

		gdk_pixbuf_composite(scaled, canvas,
		                     offset_x, offset_y,
		                     scaled_w, scaled_h,
		                     (gdouble)offset_x, (gdouble)offset_y,
		                     1.0, 1.0,
		                     GDK_INTERP_NEAREST, 255);

		g_object_unref(scaled);
		return canvas;
	}

	if (g_strcmp0(mode, "center") == 0) {
		/* Center without scaling; crop or pad with black */
		GdkPixbuf *canvas;
		gint src_x, src_y, copy_w, copy_h;
		gint dst_x, dst_y;

		canvas = gdk_pixbuf_new(GDK_COLORSPACE_RGB, TRUE, 8,
		                        mon_w, mon_h);
		if (canvas == NULL)
			return NULL;
		gdk_pixbuf_fill(canvas, 0x000000FF);

		/* Calculate source and destination offsets */
		if (img_w > mon_w) {
			src_x  = (img_w - mon_w) / 2;
			dst_x  = 0;
			copy_w = mon_w;
		} else {
			src_x  = 0;
			dst_x  = (mon_w - img_w) / 2;
			copy_w = img_w;
		}

		if (img_h > mon_h) {
			src_y  = (img_h - mon_h) / 2;
			dst_y  = 0;
			copy_h = mon_h;
		} else {
			src_y  = 0;
			dst_y  = (mon_h - img_h) / 2;
			copy_h = img_h;
		}

		gdk_pixbuf_copy_area(source, src_x, src_y, copy_w, copy_h,
		                     canvas, dst_x, dst_y);

		return canvas;
	}

	if (g_strcmp0(mode, "tile") == 0) {
		/* Tile the image across the monitor */
		GdkPixbuf *canvas;
		gint x, y;

		canvas = gdk_pixbuf_new(GDK_COLORSPACE_RGB, TRUE, 8,
		                        mon_w, mon_h);
		if (canvas == NULL)
			return NULL;
		gdk_pixbuf_fill(canvas, 0x000000FF);

		for (y = 0; y < mon_h; y += img_h) {
			for (x = 0; x < mon_w; x += img_w) {
				gint copy_w, copy_h;

				copy_w = (x + img_w > mon_w) ? (mon_w - x) : img_w;
				copy_h = (y + img_h > mon_h) ? (mon_h - y) : img_h;

				gdk_pixbuf_copy_area(source, 0, 0,
				                     copy_w, copy_h,
				                     canvas, x, y);
			}
		}

		return canvas;
	}

	/* Unknown mode: fall back to fill */
	g_warning("wallpaper: unknown mode '%s', falling back to fill", mode);
	return scale_pixbuf(source, "fill", mon_w, mon_h);
}

/* ----------------------------------------------------------------
 * Path expansion helper
 * ---------------------------------------------------------------- */

/**
 * expand_path:
 * @path: a file path that may contain ~ or environment variables
 *
 * Expands shell-like constructs in @path using wordexp.
 *
 * Returns: (transfer full): the expanded path, or a copy of @path
 *   if expansion fails
 */
static gchar *
expand_path(const gchar *path)
{
	wordexp_t result;
	gchar *expanded;

	if (path == NULL || path[0] == '\0')
		return g_strdup(path);

	if (wordexp(path, &result, WRDE_NOCMD) != 0)
		return g_strdup(path);

	if (result.we_wordc == 0) {
		wordfree(&result);
		return g_strdup(path);
	}

	expanded = g_strdup(result.we_wordv[0]);
	wordfree(&result);
	return expanded;
}

/* ----------------------------------------------------------------
 * GowlWallpaperProvider implementation
 * ---------------------------------------------------------------- */

static void
wallpaper_on_output(
	GowlWallpaperProvider *provider,
	gpointer               compositor_ptr,
	gpointer               monitor_ptr
){
	GowlModuleWallpaper *self;
	GowlCompositor *compositor;
	GowlMonitor *monitor;
	struct wlr_scene_tree *bg_layer;
	WallpaperState *state;
	const gchar *mon_name;
	gint mon_x, mon_y, mon_w, mon_h;
	g_autoptr(GError) err = NULL;
	g_autoptr(GdkPixbuf) source = NULL;
	GdkPixbuf *scaled;
	GowlPixbufBuffer *wlr_buf;
	guchar *dst_pixels;
	gint dst_stride;
	gsize dst_size;

	self = GOWL_MODULE_WALLPAPER(provider);
	compositor = GOWL_COMPOSITOR(compositor_ptr);
	monitor = GOWL_MONITOR(monitor_ptr);

	mon_name = gowl_monitor_get_name(monitor);
	gowl_monitor_get_geometry(monitor, &mon_x, &mon_y, &mon_w, &mon_h);

	/* Skip monitors with zero dimensions (not yet configured) */
	if (mon_w <= 0 || mon_h <= 0)
		return;

	bg_layer = gowl_compositor_get_scene_layer(compositor,
	                                           GOWL_SCENE_LAYER_BG);
	if (bg_layer == NULL)
		return;

	/* Remove existing wallpaper node for this monitor */
	state = (WallpaperState *)g_hash_table_lookup(self->per_monitor,
	                                              mon_name);
	if (state != NULL) {
		/* Check if the geometry hasn't changed -- skip re-render */
		if (state->width == mon_w && state->height == mon_h)
			return;

		if (state->scene_buf != NULL)
			wlr_scene_node_destroy(&state->scene_buf->node);

		g_hash_table_remove(self->per_monitor, mon_name);
		g_free(state);
	}

	/* If no path configured, leave root_bg color showing */
	if (self->path == NULL || self->path[0] == '\0')
		return;

	/* Load the image */
	source = gdk_pixbuf_new_from_file(self->path, &err);
	if (source == NULL) {
		g_warning("wallpaper: failed to load '%s': %s",
		          self->path, err->message);
		return;
	}

	/* Scale according to mode */
	scaled = scale_pixbuf(source, self->mode, mon_w, mon_h);
	if (scaled == NULL) {
		g_warning("wallpaper: failed to scale image for monitor %s",
		          mon_name);
		return;
	}

	/* Convert to ARGB8888 and create wlr_buffer */
	dst_stride = mon_w * 4;
	dst_size   = (gsize)(mon_h * dst_stride);
	dst_pixels = (guchar *)g_malloc(dst_size);
	convert_pixbuf_to_argb8888(scaled, dst_pixels, mon_w, mon_h,
	                           dst_stride);
	g_object_unref(scaled);

	wlr_buf = (GowlPixbufBuffer *)g_new0(GowlPixbufBuffer, 1);
	wlr_buf->pixels = dst_pixels;
	wlr_buf->size   = dst_size;
	wlr_buf->stride = dst_stride;
	wlr_buffer_init(&wlr_buf->base, &pixbuf_buffer_impl, mon_w, mon_h);

	/* Create scene buffer node in the BG layer */
	state = g_new0(WallpaperState, 1);
	state->scene_buf = wlr_scene_buffer_create(bg_layer, &wlr_buf->base);
	state->width     = mon_w;
	state->height    = mon_h;

	/* Position at the monitor's layout coordinates */
	wlr_scene_node_set_position(&state->scene_buf->node, mon_x, mon_y);

	/* Drop the producer ref: the scene now holds the consumer ref */
	wlr_buffer_drop(&wlr_buf->base);

	/* Store state */
	g_hash_table_insert(self->per_monitor,
	                    g_strdup(mon_name), (gpointer)state);

	g_debug("wallpaper: set for monitor %s (%dx%d+%d+%d, mode=%s)",
	        mon_name, mon_w, mon_h, mon_x, mon_y, self->mode);
}

static void
wallpaper_on_output_destroy(
	GowlWallpaperProvider *provider,
	gpointer               monitor_ptr
){
	GowlModuleWallpaper *self;
	GowlMonitor *monitor;
	WallpaperState *state;
	const gchar *mon_name;

	self = GOWL_MODULE_WALLPAPER(provider);
	monitor = GOWL_MONITOR(monitor_ptr);
	mon_name = gowl_monitor_get_name(monitor);

	state = (WallpaperState *)g_hash_table_lookup(self->per_monitor,
	                                              mon_name);
	if (state == NULL)
		return;

	/* Destroy the scene node (releases the wlr_buffer consumer ref,
	 * which triggers pixel data cleanup) */
	if (state->scene_buf != NULL)
		wlr_scene_node_destroy(&state->scene_buf->node);

	g_hash_table_remove(self->per_monitor, mon_name);
	g_free(state);

	g_debug("wallpaper: removed for monitor %s", mon_name);
}

static void
wallpaper_provider_init(GowlWallpaperProviderInterface *iface)
{
	iface->on_output         = wallpaper_on_output;
	iface->on_output_destroy = wallpaper_on_output_destroy;
}

/* ----------------------------------------------------------------
 * GowlStartupHandler implementation
 * ---------------------------------------------------------------- */

static void
wallpaper_on_startup(
	GowlStartupHandler *handler,
	gpointer            compositor
){
	GowlModuleWallpaper *self;

	self = GOWL_MODULE_WALLPAPER(handler);
	self->compositor = compositor;

	g_debug("wallpaper: startup (path=%s, mode=%s)",
	        self->path != NULL ? self->path : "(none)",
	        self->mode != NULL ? self->mode : "fill");
}

static void
wallpaper_startup_init(GowlStartupHandlerInterface *iface)
{
	iface->on_startup = wallpaper_on_startup;
}

/* ----------------------------------------------------------------
 * GowlModule virtual methods
 * ---------------------------------------------------------------- */

static gboolean
wallpaper_activate(GowlModule *mod)
{
	(void)mod;
	return TRUE;
}

static void
wallpaper_deactivate(GowlModule *mod)
{
	(void)mod;
}

static const gchar *
wallpaper_get_name(GowlModule *mod)
{
	(void)mod;
	return "wallpaper";
}

static const gchar *
wallpaper_get_description(GowlModule *mod)
{
	(void)mod;
	return "Built-in desktop wallpaper using gdk-pixbuf";
}

static const gchar *
wallpaper_get_version(GowlModule *mod)
{
	(void)mod;
	return "0.1.0";
}

static void
wallpaper_configure(GowlModule *mod, gpointer config)
{
	GowlModuleWallpaper *self;
	GHashTable *settings;
	const gchar *val;

	self = GOWL_MODULE_WALLPAPER(mod);

	if (config == NULL)
		return;

	settings = (GHashTable *)config;

	/* Image path */
	val = (const gchar *)g_hash_table_lookup(settings, "path");
	if (val != NULL) {
		g_free(self->path);
		self->path = expand_path(val);
	}

	/* Scaling mode */
	val = (const gchar *)g_hash_table_lookup(settings, "mode");
	if (val != NULL) {
		g_free(self->mode);
		self->mode = g_strdup(val);
	}

	g_message("wallpaper: configured (path=%s, mode=%s)",
	          self->path != NULL ? self->path : "(none)",
	          self->mode != NULL ? self->mode : "fill");
}

/* ----------------------------------------------------------------
 * GObject lifecycle
 * ---------------------------------------------------------------- */

static void
gowl_module_wallpaper_finalize(GObject *object)
{
	GowlModuleWallpaper *self;
	GHashTableIter iter;
	gpointer key, value;

	self = GOWL_MODULE_WALLPAPER(object);

	/* Destroy remaining scene nodes */
	if (self->per_monitor != NULL) {
		g_hash_table_iter_init(&iter, self->per_monitor);
		while (g_hash_table_iter_next(&iter, &key, &value)) {
			WallpaperState *state;

			state = (WallpaperState *)value;
			if (state->scene_buf != NULL)
				wlr_scene_node_destroy(&state->scene_buf->node);
			g_free(state);
		}
		g_hash_table_destroy(self->per_monitor);
	}

	g_free(self->path);
	g_free(self->mode);

	G_OBJECT_CLASS(gowl_module_wallpaper_parent_class)->finalize(object);
}

static void
gowl_module_wallpaper_class_init(GowlModuleWallpaperClass *klass)
{
	GObjectClass    *object_class;
	GowlModuleClass *mod_class;

	object_class = G_OBJECT_CLASS(klass);
	mod_class    = GOWL_MODULE_CLASS(klass);

	object_class->finalize = gowl_module_wallpaper_finalize;

	mod_class->activate        = wallpaper_activate;
	mod_class->deactivate      = wallpaper_deactivate;
	mod_class->get_name        = wallpaper_get_name;
	mod_class->get_description = wallpaper_get_description;
	mod_class->get_version     = wallpaper_get_version;
	mod_class->configure       = wallpaper_configure;
}

static void
gowl_module_wallpaper_init(GowlModuleWallpaper *self)
{
	self->path         = g_strdup("");
	self->mode         = g_strdup("fill");
	self->compositor   = NULL;
	self->per_monitor  = g_hash_table_new_full(g_str_hash, g_str_equal,
	                                           g_free, NULL);
}

/* ----------------------------------------------------------------
 * Shared-object entry point
 * ---------------------------------------------------------------- */

G_MODULE_EXPORT GType
gowl_module_register(void)
{
	return GOWL_TYPE_MODULE_WALLPAPER;
}
