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
 * GowlModuleRoundCorners:
 *
 * Replaces the default flat rectangular window borders with
 * cairo-rendered rounded rectangle frames.  Each client gets a
 * single wlr_scene_buffer that draws a rounded border stroke
 * with transparent interior.
 *
 * Configuration (YAML):
 *   modules:
 *     roundcorners:
 *       enabled: true
 *       radius: 12
 */

#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "gowl-roundcorners"

#include <glib-object.h>
#include <gmodule.h>
#include <math.h>
#include <string.h>

#include <cairo.h>
#include <drm_fourcc.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/types/wlr_scene.h>

#include "gowl-enums.h"
#include "module/gowl-module.h"
#include "interfaces/gowl-client-decorator.h"
#include "interfaces/gowl-startup-handler.h"
#include "interfaces/gowl-shutdown-handler.h"
#include "core/gowl-compositor.h"
#include "core/gowl-client.h"

/* ----------------------------------------------------------------
 * Custom wlr_buffer for rounded border pixel data
 * ---------------------------------------------------------------- */

typedef struct {
	struct wlr_buffer base;
	guchar *pixels;
	gsize   size;
	gint    stride;
} RoundBuffer;

static void
round_buffer_destroy(struct wlr_buffer *wlr_buf)
{
	RoundBuffer *buf = wl_container_of(wlr_buf, buf, base);
	g_free(buf->pixels);
	g_free(buf);
}

static bool
round_buffer_begin_data_ptr_access(struct wlr_buffer *wlr_buf,
                                   uint32_t flags,
                                   void **data,
                                   uint32_t *format,
                                   size_t *stride)
{
	RoundBuffer *buf = wl_container_of(wlr_buf, buf, base);
	*data   = buf->pixels;
	*format = DRM_FORMAT_ARGB8888;
	*stride = (size_t)buf->stride;
	return true;
}

static void
round_buffer_end_data_ptr_access(struct wlr_buffer *wlr_buf)
{
	(void)wlr_buf;
}

static const struct wlr_buffer_impl round_buffer_impl = {
	.destroy = round_buffer_destroy,
	.begin_data_ptr_access = round_buffer_begin_data_ptr_access,
	.end_data_ptr_access = round_buffer_end_data_ptr_access,
};

/* ----------------------------------------------------------------
 * Per-client decoration state
 * ---------------------------------------------------------------- */

typedef struct {
	struct wlr_scene_buffer *frame_buf;
	gint    width;
	gint    height;
	guint   bw;
	float   color[4];
} RoundDecor;

/* Pointer hit-test callback for the decoration buffer. Always
   reports "not hit" so the cairo stroke pixels never block clicks
   to the client surface beneath -- critical for embedded clients
   where the decoration can cover a large area. */
static bool
rc_no_input(struct wlr_scene_buffer *buffer, double *sx, double *sy)
{
	(void)buffer;
	(void)sx;
	(void)sy;
	return false;
}

/* ----------------------------------------------------------------
 * Module type
 * ---------------------------------------------------------------- */

#define GOWL_TYPE_MODULE_ROUND_CORNERS (gowl_module_round_corners_get_type())

G_DECLARE_FINAL_TYPE(GowlModuleRoundCorners, gowl_module_round_corners,
                     GOWL, MODULE_ROUND_CORNERS, GowlModule)

struct _GowlModuleRoundCorners {
	GowlModule parent_instance;

	GowlCompositor *compositor;
	gint            corner_radius;
	GHashTable     *decorations;  /* GowlClient* → RoundDecor* */
};

static void rc_decorator_init  (GowlClientDecoratorInterface *iface);
static void rc_startup_init    (GowlStartupHandlerInterface *iface);
static void rc_shutdown_init   (GowlShutdownHandlerInterface *iface);
static void rc_destroy_decoration (GowlClientDecorator *decorator,
                                   gpointer             client_ptr);

G_DEFINE_TYPE_WITH_CODE(GowlModuleRoundCorners, gowl_module_round_corners,
    GOWL_TYPE_MODULE,
    G_IMPLEMENT_INTERFACE(GOWL_TYPE_CLIENT_DECORATOR, rc_decorator_init)
    G_IMPLEMENT_INTERFACE(GOWL_TYPE_STARTUP_HANDLER, rc_startup_init)
    G_IMPLEMENT_INTERFACE(GOWL_TYPE_SHUTDOWN_HANDLER, rc_shutdown_init))

/* ----------------------------------------------------------------
 * Cairo rendering: rounded rectangle border frame
 * ---------------------------------------------------------------- */

static RoundBuffer *
render_rounded_frame(gint width, gint height, guint bw,
                     gint radius, const float color[4])
{
	cairo_surface_t *cs;
	cairo_t *cr;
	RoundBuffer *buf;
	guchar *pixels;
	gint stride;
	gdouble r, half_bw;
	gdouble x0, y0, x1, y1;

	if (width <= 0 || height <= 0 || bw == 0)
		return NULL;

	cs = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
	cr = cairo_create(cs);

	/* Clear to transparent */
	cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
	cairo_set_source_rgba(cr, 0, 0, 0, 0);
	cairo_paint(cr);
	cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

	r = (gdouble)radius;
	half_bw = (gdouble)bw / 2.0;

	/* Inset the rounded rect path by half the border width so
	 * the stroke is centered on the intended border area */
	x0 = half_bw;
	y0 = half_bw;
	x1 = (gdouble)width - half_bw;
	y1 = (gdouble)height - half_bw;

	/* Clamp radius to half the available space */
	if (r > (x1 - x0) / 2.0)
		r = (x1 - x0) / 2.0;
	if (r > (y1 - y0) / 2.0)
		r = (y1 - y0) / 2.0;
	if (r < 0)
		r = 0;

	/* Build the rounded rectangle path */
	cairo_new_sub_path(cr);
	/* Top-left arc */
	cairo_arc(cr, x0 + r, y0 + r, r, M_PI, 3.0 * M_PI / 2.0);
	/* Top-right arc */
	cairo_arc(cr, x1 - r, y0 + r, r, 3.0 * M_PI / 2.0, 2.0 * M_PI);
	/* Bottom-right arc */
	cairo_arc(cr, x1 - r, y1 - r, r, 0, M_PI / 2.0);
	/* Bottom-left arc */
	cairo_arc(cr, x0 + r, y1 - r, r, M_PI / 2.0, M_PI);
	cairo_close_path(cr);

	/* Stroke with the border color and width */
	cairo_set_line_width(cr, (gdouble)bw);
	cairo_set_source_rgba(cr, (gdouble)color[0], (gdouble)color[1],
	                      (gdouble)color[2], (gdouble)color[3]);
	cairo_stroke(cr);

	/* Copy to pixel buffer */
	cairo_surface_flush(cs);
	stride = cairo_image_surface_get_stride(cs);
	pixels = (guchar *)g_malloc((gsize)(stride * height));
	memcpy(pixels, cairo_image_surface_get_data(cs), (gsize)(stride * height));
	cairo_destroy(cr);
	cairo_surface_destroy(cs);

	buf = g_new0(RoundBuffer, 1);
	buf->pixels = pixels;
	buf->size   = (gsize)(stride * height);
	buf->stride = stride;
	wlr_buffer_init(&buf->base, &round_buffer_impl, width, height);

	return buf;
}

/* ----------------------------------------------------------------
 * GowlClientDecorator interface
 * ---------------------------------------------------------------- */

static void
rc_render_decoration(GowlClientDecorator *decorator,
                     gpointer             client_ptr,
                     gint                 width,
                     gint                 height,
                     guint                bw,
                     const float         *color)
{
	GowlModuleRoundCorners *self = GOWL_MODULE_ROUND_CORNERS(decorator);
	GowlClient *client = GOWL_CLIENT(client_ptr);
	RoundDecor *decor;
	RoundBuffer *buf;
	struct wlr_scene_tree *scene;

	/* Degenerate/borderless case: nothing to draw.  Make sure any
	   pre-existing decoration for this client is destroyed so a
	   stale buffer doesn't linger with old dimensions (critical for
	   embedded clients where bw is forced to 0). */
	if (width <= 0 || height <= 0 || bw == 0) {
		rc_destroy_decoration(decorator, client_ptr);
		return;
	}

	decor = (RoundDecor *)g_hash_table_lookup(self->decorations, client);

	/* Check if we can skip re-rendering (size/color unchanged) */
	if (decor != NULL && decor->width == width && decor->height == height
	    && decor->bw == bw
	    && decor->color[0] == color[0] && decor->color[1] == color[1]
	    && decor->color[2] == color[2] && decor->color[3] == color[3]) {
		return;
	}

	buf = render_rounded_frame(width, height, bw,
	                           self->corner_radius, color);
	if (buf == NULL)
		return;

	if (decor == NULL) {
		/* First decoration for this client */
		decor = g_new0(RoundDecor, 1);

		scene = gowl_client_get_scene(client);
		if (scene == NULL) {
			g_free(decor);
			wlr_buffer_drop(&buf->base);
			return;
		}

		decor->frame_buf = wlr_scene_buffer_create(scene, &buf->base);
		wlr_scene_node_set_position(&decor->frame_buf->node, 0, 0);
		/* Make the decoration transparent to pointer input so clicks
		   pass through to the client surface underneath.  Without
		   this, the opaque cairo stroke pixels eat pointer events
		   anywhere they're drawn, even though they're visually a
		   thin border. */
		decor->frame_buf->point_accepts_input = rc_no_input;

		g_hash_table_insert(self->decorations,
		                    g_object_ref(client), decor);
	} else {
		/* Update existing buffer */
		wlr_scene_buffer_set_buffer(decor->frame_buf, &buf->base);
	}

	wlr_buffer_drop(&buf->base);

	decor->width    = width;
	decor->height   = height;
	decor->bw       = bw;
	decor->color[0] = color[0];
	decor->color[1] = color[1];
	decor->color[2] = color[2];
	decor->color[3] = color[3];
}

static void
rc_destroy_decoration(GowlClientDecorator *decorator,
                      gpointer             client_ptr)
{
	GowlModuleRoundCorners *self = GOWL_MODULE_ROUND_CORNERS(decorator);
	GowlClient *client = GOWL_CLIENT(client_ptr);
	RoundDecor *decor;

	decor = (RoundDecor *)g_hash_table_lookup(self->decorations, client);
	if (decor == NULL)
		return;

	if (decor->frame_buf != NULL)
		wlr_scene_node_destroy(&decor->frame_buf->node);

	g_hash_table_remove(self->decorations, client);
	/* Release the extra reference taken when the decoration was
	 * inserted (see g_object_ref in rc_render_decoration).  Without
	 * this the GowlClient leaks one ref per decoration, since
	 * on_client_destroy's g_object_unref(c) only drops the
	 * compositor's ownership ref -- the decorator's ref would pin it. */
	g_object_unref(client);
	g_free(decor);
}

static gint
rc_get_corner_radius(GowlClientDecorator *decorator)
{
	return GOWL_MODULE_ROUND_CORNERS(decorator)->corner_radius;
}

static gint
rc_get_border_width(GowlClientDecorator *decorator, gpointer client)
{
	(void)decorator;
	(void)client;
	return -1; /* -1 = use compositor default */
}

static gboolean
rc_should_draw_border(GowlClientDecorator *decorator, gpointer client)
{
	(void)decorator;
	(void)client;
	return TRUE;
}

static void
rc_decorator_init(GowlClientDecoratorInterface *iface)
{
	iface->render_decoration  = rc_render_decoration;
	iface->destroy_decoration = rc_destroy_decoration;
	iface->get_corner_radius  = rc_get_corner_radius;
	iface->get_border_width   = rc_get_border_width;
	iface->should_draw_border = rc_should_draw_border;
}

/* ----------------------------------------------------------------
 * GowlStartupHandler
 * ---------------------------------------------------------------- */

static void
rc_on_startup(GowlStartupHandler *handler, gpointer compositor)
{
	GowlModuleRoundCorners *self = GOWL_MODULE_ROUND_CORNERS(handler);
	self->compositor = GOWL_COMPOSITOR(compositor);
}

static void
rc_startup_init(GowlStartupHandlerInterface *iface)
{
	iface->on_startup = rc_on_startup;
}

/* ----------------------------------------------------------------
 * GowlShutdownHandler
 * ---------------------------------------------------------------- */

static void
rc_destroy_decor_entry(gpointer key, gpointer value, gpointer user_data)
{
	RoundDecor *decor = (RoundDecor *)value;

	(void)user_data;

	if (decor->frame_buf != NULL)
		wlr_scene_node_destroy(&decor->frame_buf->node);
	/* Match the g_object_ref taken at insert (key is the GowlClient). */
	g_object_unref(GOWL_CLIENT(key));
	g_free(decor);
}

static void
rc_on_shutdown(GowlShutdownHandler *handler, gpointer compositor)
{
	GowlModuleRoundCorners *self = GOWL_MODULE_ROUND_CORNERS(handler);

	(void)compositor;

	g_hash_table_foreach(self->decorations, rc_destroy_decor_entry, NULL);
	g_hash_table_remove_all(self->decorations);
	self->compositor = NULL;
}

static void
rc_shutdown_init(GowlShutdownHandlerInterface *iface)
{
	iface->on_shutdown = rc_on_shutdown;
}

/* ----------------------------------------------------------------
 * GowlModule virtual methods
 * ---------------------------------------------------------------- */

static gboolean
rc_activate(GowlModule *mod)
{
	(void)mod;
	return TRUE;
}

static void
rc_deactivate(GowlModule *mod)
{
	GowlModuleRoundCorners *self = GOWL_MODULE_ROUND_CORNERS(mod);

	g_hash_table_foreach(self->decorations, rc_destroy_decor_entry, NULL);
	g_hash_table_remove_all(self->decorations);
}

static const gchar *
rc_get_name(GowlModule *mod)
{
	(void)mod;
	return "roundcorners";
}

static const gchar *
rc_get_description(GowlModule *mod)
{
	(void)mod;
	return "Rounded corner window borders via cairo";
}

static const gchar *
rc_get_version(GowlModule *mod)
{
	(void)mod;
	return "0.1.0";
}

static void
rc_configure(GowlModule *mod, gpointer config)
{
	GowlModuleRoundCorners *self = GOWL_MODULE_ROUND_CORNERS(mod);
	GHashTable *settings;
	const gchar *val;

	if (config == NULL)
		return;

	settings = (GHashTable *)config;

	val = g_hash_table_lookup(settings, "radius");
	if (val != NULL) {
		gint r = (gint)g_ascii_strtoll(val, NULL, 10);
		if (r >= 0 && r <= 100)
			self->corner_radius = r;
	}

	/* Force re-render all decorations with new radius */
	if (self->compositor != NULL) {
		GHashTableIter iter;
		gpointer key, value;

		g_hash_table_iter_init(&iter, self->decorations);
		while (g_hash_table_iter_next(&iter, &key, &value)) {
			RoundDecor *decor = (RoundDecor *)value;
			/* Invalidate cached size to force re-render */
			decor->width = 0;
		}
	}
}

/* ----------------------------------------------------------------
 * GObject lifecycle
 * ---------------------------------------------------------------- */

static void
gowl_module_round_corners_finalize(GObject *object)
{
	GowlModuleRoundCorners *self = GOWL_MODULE_ROUND_CORNERS(object);

	g_hash_table_foreach(self->decorations, rc_destroy_decor_entry, NULL);
	g_hash_table_unref(self->decorations);

	G_OBJECT_CLASS(gowl_module_round_corners_parent_class)->finalize(object);
}

static void
gowl_module_round_corners_class_init(GowlModuleRoundCornersClass *klass)
{
	GObjectClass    *obj_class = G_OBJECT_CLASS(klass);
	GowlModuleClass *mod_class = GOWL_MODULE_CLASS(klass);

	obj_class->finalize = gowl_module_round_corners_finalize;

	mod_class->activate       = rc_activate;
	mod_class->deactivate     = rc_deactivate;
	mod_class->get_name       = rc_get_name;
	mod_class->get_description = rc_get_description;
	mod_class->get_version    = rc_get_version;
	mod_class->configure      = rc_configure;
}

static void
gowl_module_round_corners_init(GowlModuleRoundCorners *self)
{
	self->compositor    = NULL;
	self->corner_radius = 12;
	self->decorations   = g_hash_table_new(g_direct_hash, g_direct_equal);
}

/* ----------------------------------------------------------------
 * Module entry point
 * ---------------------------------------------------------------- */

G_MODULE_EXPORT GType
gowl_module_register(void)
{
	return GOWL_TYPE_MODULE_ROUND_CORNERS;
}
