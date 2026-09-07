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
#include <wlr/util/box.h>

#include "gowl-enums.h"
#include "module/gowl-module.h"
#include "interfaces/gowl-scene-effect.h"
#include "interfaces/gowl-startup-handler.h"
#include "interfaces/gowl-wallpaper-provider.h"
#include "config/gowl-config.h"
#include "core/gowl-core-private.h"
#include "core/gowl-compositor.h"
#include "core/gowl-monitor.h"
#include "util/gowl-wallpaper-scale.h"

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

	/*
	 * Per-tag wallpaper.
	 *
	 * `wallpaper' in the config stays what it always was: the picture
	 * every tag uses.  A tag with an entry in `wallpaper-tags' overrides
	 * it, and one without simply keeps the default -- so an existing
	 * config is untouched and a user can override one tag without
	 * declaring the other eight.
	 */
	gchar   *shown_path;      /* what scene_buf is currently showing */
	guint32  shown_tags;      /* the tag set it was chosen for */

	/*
	 * A cross-fade in flight.  The incoming picture is a second node
	 * stacked above the outgoing one and faded up, rather than one node
	 * whose buffer is swapped: a swap is a cut, and the whole point of
	 * the setting is that it should not be.
	 */
	struct wlr_scene_buffer *fading_buf;
	gchar   *fading_path;
	gint64   fade_start_us;
	gint64   fade_dur_us;
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

	/* Decode cache.  The image is decoded once and reused on every
	 * resize, so a geometry change re-scales it in memory (on the CPU,
	 * via scale_pixbuf()) but never re-reads the file. */
	/* Every wallpaper the config mentions, decoded once.  With per-tag
	 * wallpapers the single-entry cache above would thrash on every tag
	 * switch, re-reading a file from disk to show a picture it decoded a
	 * moment ago. */
	GHashTable *decoded;     /* path -> GdkPixbuf* */
};

/* Forward declarations for interface init functions */
static void wallpaper_startup_init  (GowlStartupHandlerInterface *iface);
static void wallpaper_provider_init (GowlWallpaperProviderInterface *iface);
static void wallpaper_effect_init   (GowlSceneEffectInterface *iface);

/*
 * The scene-effect interface is implemented only for its per-frame tick,
 * which is what a cross-fade needs and what a wallpaper provider has no
 * other way to get.  Nothing here claims an event, so window animations
 * are entirely unaffected by this module being loaded.
 */
G_DEFINE_TYPE_WITH_CODE(GowlModuleWallpaper, gowl_module_wallpaper,
	GOWL_TYPE_MODULE,
	G_IMPLEMENT_INTERFACE(GOWL_TYPE_STARTUP_HANDLER,
		wallpaper_startup_init)
	G_IMPLEMENT_INTERFACE(GOWL_TYPE_SCENE_EFFECT,
		wallpaper_effect_init)
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

		gowl_wallpaper_cover_rect(img_w, img_h, mon_w, mon_h,
		                          &scaled_w, &scaled_h,
		                          &crop_x, &crop_y);

		scaled = gdk_pixbuf_scale_simple(source, scaled_w, scaled_h,
		                                 GDK_INTERP_BILINEAR);
		if (scaled == NULL)
			return NULL;

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

		gowl_wallpaper_fit_rect(img_w, img_h, mon_w, mon_h,
		                        &scaled_w, &scaled_h,
		                        &offset_x, &offset_y);

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

		gowl_wallpaper_center_rect(img_w, img_h, mon_w, mon_h,
		                           &src_x, &src_y, &dst_x, &dst_y,
		                           &copy_w, &copy_h);

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
 * Decode cache
 * ---------------------------------------------------------------- */

/*
 * Decode a wallpaper once and keep it.
 *
 * Every scaling mode is rendered on the CPU from the cached pixbuf (see
 * scale_pixbuf()), so a resize re-scales in memory but never re-reads the
 * file -- and with per-tag wallpapers, neither does switching back to a
 * tag whose picture was decoded a minute ago.
 *
 * Returns: (transfer none) (nullable): the decoded image.
 */
static void
wallpaper_unref_maybe(gpointer pixbuf)
{
	if (pixbuf != NULL)
		g_object_unref(pixbuf);
}

static GdkPixbuf *
wallpaper_decode(GowlModuleWallpaper *self, const gchar *path)
{
	g_autoptr(GError) err = NULL;
	GdkPixbuf *pixbuf;

	if (path == NULL || path[0] == '\0')
		return NULL;

	pixbuf = g_hash_table_lookup(self->decoded, path);
	if (pixbuf != NULL)
		return pixbuf;

	pixbuf = gdk_pixbuf_new_from_file(path, &err);
	if (pixbuf == NULL) {
		/* Cache the failure as a NULL so a broken path in the config is
		 * not re-opened on every tag switch for the rest of the
		 * session. */
		g_warning("wallpaper: failed to load '%s': %s", path, err->message);
		g_hash_table_insert(self->decoded, g_strdup(path), NULL);
		return NULL;
	}

	g_hash_table_insert(self->decoded, g_strdup(path), pixbuf);
	return pixbuf;
}

/*
 * The wallpaper a monitor should be showing right now.
 *
 * The lowest set tag decides, the same rule the cube uses to pick a face:
 * a combined tag view has no single honest answer, and taking the lowest
 * means adding a tag to a view does not change the wallpaper while
 * replacing one does.
 *
 * Returns: (transfer none) (nullable): a path, or %NULL for none at all.
 */
static const gchar *
wallpaper_path_for(GowlModuleWallpaper *self, GowlCompositor *comp,
                    GowlMonitor *monitor)
{
	guint32 tags;
	gint    i;

	if (comp == NULL || comp->config == NULL || monitor == NULL)
		return self->path;

	tags = monitor->tagset[monitor->seltags];
	for (i = 0; i < GOWL_CONFIG_MAX_TAGS; i++) {
		if ((tags & (1u << i)) != 0) {
			const gchar *override =
				gowl_config_get_wallpaper_for_tag(comp->config, i + 1);

			if (override != NULL && override[0] != '\0')
				return override;
			break;
		}
	}
	return self->path;
}

/* ----------------------------------------------------------------
 * GowlWallpaperProvider implementation
 * ---------------------------------------------------------------- */

/*
 * Build one monitor-sized scene node showing @path.
 *
 * Scaling happens on the CPU into an exact monitor-sized buffer -- one
 * proven path for every mode (fill/fit/center/stretch/tile).  The GPU is
 * NOT handed the full-resolution source plus a source box: a wallpaper
 * larger than the driver's maximum texture size tiled into a grid on some
 * hardware, which is the sort of bug that only appears on somebody else's
 * machine.
 *
 * Returns: (transfer none) (nullable): the new node.
 */
static struct wlr_scene_buffer *
wallpaper_make_node(GowlModuleWallpaper *self, struct wlr_scene_tree *bg_layer,
                     const gchar *path, gint x, gint y, gint width, gint height)
{
	GdkPixbuf *source = wallpaper_decode(self, path);
	GdkPixbuf *scaled;
	GowlPixbufBuffer *wlr_buf;
	struct wlr_scene_buffer *node;
	guchar *dst_pixels;
	gint dst_stride;
	gsize dst_size;

	if (source == NULL || bg_layer == NULL || width <= 0 || height <= 0)
		return NULL;

	scaled = scale_pixbuf(source, self->mode, width, height);
	if (scaled == NULL)
		return NULL;

	dst_stride = width * 4;
	dst_size   = (gsize)height * dst_stride;
	dst_pixels = (guchar *)g_malloc(dst_size);
	convert_pixbuf_to_argb8888(scaled, dst_pixels, width, height, dst_stride);
	g_object_unref(scaled);

	wlr_buf = (GowlPixbufBuffer *)g_new0(GowlPixbufBuffer, 1);
	wlr_buf->pixels = dst_pixels;
	wlr_buf->size   = dst_size;
	wlr_buf->stride = dst_stride;
	wlr_buffer_init(&wlr_buf->base, &pixbuf_buffer_impl, width, height);

	node = wlr_scene_buffer_create(bg_layer, &wlr_buf->base);
	/* The scene now holds the only reference to this buffer. */
	wlr_buffer_drop(&wlr_buf->base);
	if (node != NULL)
		wlr_scene_node_set_position(&node->node, x, y);
	return node;
}

/* Finish a cross-fade: the incoming picture becomes the wallpaper and the
 * outgoing one goes. */
static void
wallpaper_settle(WallpaperState *state)
{
	if (state->fading_buf == NULL)
		return;

	if (state->scene_buf != NULL)
		wlr_scene_node_destroy(&state->scene_buf->node);
	state->scene_buf = state->fading_buf;
	wlr_scene_buffer_set_opacity(state->scene_buf, 1.0f);
	state->fading_buf = NULL;

	g_free(state->shown_path);
	state->shown_path  = state->fading_path;
	state->fading_path = NULL;
}

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
	const gchar *path;
	gint mon_x, mon_y, mon_w, mon_h;

	self = GOWL_MODULE_WALLPAPER(provider);
	compositor = GOWL_COMPOSITOR(compositor_ptr);
	monitor = GOWL_MONITOR(monitor_ptr);

	self->compositor = compositor;

	mon_name = gowl_monitor_get_name(monitor);
	gowl_monitor_get_geometry(monitor, &mon_x, &mon_y, &mon_w, &mon_h);

	/* Skip monitors with zero dimensions (not yet configured) */
	if (mon_w <= 0 || mon_h <= 0)
		return;

	bg_layer = gowl_compositor_get_scene_layer(compositor,
	                                           GOWL_SCENE_LAYER_BG);
	if (bg_layer == NULL)
		return;

	path = wallpaper_path_for(self, compositor, monitor);

	state = (WallpaperState *)g_hash_table_lookup(self->per_monitor,
	                                              mon_name);
	/* Already correct at this size and showing this picture: the common
	 * case during the run, and it must stay free. */
	if (state != NULL && state->width == mon_w && state->height == mon_h
	    && g_strcmp0(state->shown_path, path) == 0
	    && state->fading_buf == NULL)
		return;

	/* Geometry changed (or first time): tear down the old node. */
	if (state != NULL) {
		if (state->fading_buf != NULL)
			wlr_scene_node_destroy(&state->fading_buf->node);
		if (state->scene_buf != NULL)
			wlr_scene_node_destroy(&state->scene_buf->node);
		g_free(state->shown_path);
		g_free(state->fading_path);
		g_hash_table_remove(self->per_monitor, mon_name);
		state = NULL;
	}

	/* If no path applies, leave root_bg color showing */
	if (path == NULL || path[0] == '\0')
		return;

	state = g_new0(WallpaperState, 1);
	state->width  = mon_w;
	state->height = mon_h;
	state->scene_buf = wallpaper_make_node(self, bg_layer, path,
	                                       mon_x, mon_y, mon_w, mon_h);
	if (state->scene_buf == NULL) {
		g_free(state);
		return;
	}
	state->shown_path = g_strdup(path);
	state->shown_tags = monitor->tagset[monitor->seltags];

	g_hash_table_insert(self->per_monitor,
	                    g_strdup(mon_name), (gpointer)state);

	g_debug("wallpaper: set for monitor %s (%dx%d+%d+%d, mode=%s, path=%s)",
	        mon_name, mon_w, mon_h, mon_x, mon_y, self->mode, path);
}

/*
 * A tag change, noticed per frame.
 *
 * Polling rather than listening for the monitor's tag-changed signal, for
 * the same reason the cube polls: the module has to be able to cope with
 * an output that appeared after it loaded, and a per-monitor signal
 * connection would miss those.  It costs one integer comparison per
 * output per frame, and only when per-tag wallpapers are configured at
 * all.
 */
static gboolean
wallpaper_frame(GowlSceneEffect *effect, GowlCompositor *comp,
                 GowlMonitor *monitor, gint64 now)
{
	GowlModuleWallpaper *self = GOWL_MODULE_WALLPAPER(effect);
	WallpaperState *state;
	struct wlr_scene_tree *bg_layer;
	const gchar *mon_name;
	const gchar *path;
	gint mon_x, mon_y, mon_w, mon_h;
	gint fade_ms;

	if (comp == NULL || monitor == NULL || comp->config == NULL)
		return FALSE;

	self->compositor = comp;

	mon_name = gowl_monitor_get_name(monitor);
	state = mon_name != NULL
		? (WallpaperState *)g_hash_table_lookup(self->per_monitor, mon_name)
		: NULL;
	if (state == NULL)
		return FALSE;

	/* Advance a fade already running. */
	if (state->fading_buf != NULL) {
		gdouble t = state->fade_dur_us > 0
			? CLAMP((gdouble)(now - state->fade_start_us)
			        / (gdouble)state->fade_dur_us, 0.0, 1.0)
			: 1.0;

		wlr_scene_buffer_set_opacity(state->fading_buf, (gfloat)t);
		if (t >= 1.0) {
			wallpaper_settle(state);
			return FALSE;
		}
		return TRUE;   /* keep the frames coming while it fades */
	}

	if (!gowl_config_has_tag_wallpapers(comp->config))
		return FALSE;

	if (state->shown_tags == monitor->tagset[monitor->seltags])
		return FALSE;
	state->shown_tags = monitor->tagset[monitor->seltags];

	path = wallpaper_path_for(self, comp, monitor);
	if (path == NULL || g_strcmp0(path, state->shown_path) == 0)
		return FALSE;

	gowl_monitor_get_geometry(monitor, &mon_x, &mon_y, &mon_w, &mon_h);
	bg_layer = gowl_compositor_get_scene_layer(comp, GOWL_SCENE_LAYER_BG);
	if (bg_layer == NULL || mon_w <= 0 || mon_h <= 0)
		return FALSE;

	state->fading_buf = wallpaper_make_node(self, bg_layer, path,
	                                        mon_x, mon_y, mon_w, mon_h);
	if (state->fading_buf == NULL)
		return FALSE;

	g_free(state->fading_path);
	state->fading_path = g_strdup(path);

	/* Above the outgoing picture, so fading it up reveals it rather than
	 * revealing whatever is under the wallpaper. */
	wlr_scene_node_place_above(&state->fading_buf->node,
	                           &state->scene_buf->node);

	fade_ms = gowl_config_get_wallpaper_fade(comp->config);
	if (fade_ms <= 0) {
		wlr_scene_buffer_set_opacity(state->fading_buf, 1.0f);
		wallpaper_settle(state);
		return FALSE;
	}

	wlr_scene_buffer_set_opacity(state->fading_buf, 0.0f);
	state->fade_start_us = now;
	state->fade_dur_us   = (gint64)fade_ms * 1000;
	return TRUE;
}

/*
 * Wallpapers are scene nodes, not held buffers or GL objects, so there is
 * nothing here that must go before the renderer does -- but a fade caught
 * mid-flight by a shutdown would leave two nodes stacked, so it is
 * settled rather than abandoned.
 */
static void
wallpaper_effect_finish(GowlSceneEffect *effect, GowlCompositor *comp)
{
	GowlModuleWallpaper *self = GOWL_MODULE_WALLPAPER(effect);
	GHashTableIter iter;
	gpointer key, value;

	if (self->per_monitor == NULL)
		return;

	g_hash_table_iter_init(&iter, self->per_monitor);
	while (g_hash_table_iter_next(&iter, &key, &value))
		wallpaper_settle((WallpaperState *)value);
}

static void
wallpaper_effect_init(GowlSceneEffectInterface *iface)
{
	iface->frame  = wallpaper_frame;
	iface->finish = wallpaper_effect_finish;
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

	/* Destroy the scene nodes (releases the wlr_buffer consumer ref,
	 * which triggers pixel data cleanup).  A cross-fade caught in flight
	 * has two of them. */
	if (state->fading_buf != NULL)
		wlr_scene_node_destroy(&state->fading_buf->node);
	if (state->scene_buf != NULL)
		wlr_scene_node_destroy(&state->scene_buf->node);

	g_hash_table_remove(self->per_monitor, mon_name);
	g_free(state->shown_path);
	g_free(state->fading_path);
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
			if (state->fading_buf != NULL)
				wlr_scene_node_destroy(&state->fading_buf->node);
			if (state->scene_buf != NULL)
				wlr_scene_node_destroy(&state->scene_buf->node);
			g_free(state->shown_path);
			g_free(state->fading_path);
			g_free(state);
		}
		g_hash_table_destroy(self->per_monitor);
	}

	g_clear_pointer(&self->decoded, g_hash_table_unref);

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
	/* A path that failed to decode is cached as a NULL value so it is not
	 * re-opened on every tag switch, and GHashTable calls the value
	 * destructor even for those -- hence the wrapper, because
	 * g_object_unref(NULL) is a critical warning rather than a no-op. */
	self->decoded      = g_hash_table_new_full(g_str_hash, g_str_equal,
	                                           g_free, wallpaper_unref_maybe);
}

/* ----------------------------------------------------------------
 * Shared-object entry point
 * ---------------------------------------------------------------- */

G_MODULE_EXPORT GType
gowl_module_register(void)
{
	return GOWL_TYPE_MODULE_WALLPAPER;
}
