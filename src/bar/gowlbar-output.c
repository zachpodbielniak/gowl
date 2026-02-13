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

#define _GNU_SOURCE
#include "gowlbar-output.h"

#include <glib.h>
#include <wayland-client.h>
#include <cairo/cairo.h>
#include <pango/pangocairo.h>
#include <string.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>
#include <errno.h>

#include "wlr-layer-shell-unstable-v1-client-protocol.h"

/**
 * GowlbarOutput:
 *
 * Represents a bar surface on a single output (monitor).
 * Manages the wl_surface, layer surface, wl_shm buffer pool,
 * cairo rendering surface, and widget layout/rendering pipeline.
 */
struct _GowlbarOutput {
	GObject parent_instance;

	struct wl_output              *wl_output;
	gchar                         *name;
	guint32                        global_name;

	/* wayland surface objects */
	struct wl_surface             *wl_surface;
	struct zwlr_layer_surface_v1  *layer_surface;
	struct wl_shm                 *wl_shm;

	/* buffer management */
	struct wl_buffer              *buffer;
	void                          *shm_data;
	gint                           shm_size;
	gint                           shm_fd;

	/* bar dimensions */
	gint                           width;
	gint                           height;
	gint                           stride;

	/* configuration and widgets */
	GowlbarConfig                 *config;   /* borrowed ref */
	GList                         *widgets;  /* borrowed GList of GowlbarWidget* */

	/* state */
	gboolean                       configured;
};

G_DEFINE_FINAL_TYPE(GowlbarOutput, gowlbar_output, G_TYPE_OBJECT)

/* --- Forward declarations --- */

static void layer_surface_configure(void *data,
                                    struct zwlr_layer_surface_v1 *surface,
                                    uint32_t serial,
                                    uint32_t width,
                                    uint32_t height);
static void layer_surface_closed(void *data,
                                 struct zwlr_layer_surface_v1 *surface);

static const struct zwlr_layer_surface_v1_listener layer_surface_listener = {
	.configure = layer_surface_configure,
	.closed    = layer_surface_closed,
};

/* --- Colour parsing helper --- */

/**
 * parse_hex_color:
 * @hex: hex colour string like "#rrggbb"
 * @r: (out): red 0.0–1.0
 * @g: (out): green 0.0–1.0
 * @b: (out): blue 0.0–1.0
 *
 * Parses a hex colour string into normalised RGB values.
 */
static void
parse_hex_color(const gchar *hex, gdouble *r, gdouble *g, gdouble *b)
{
	guint32 val;

	if (hex == NULL || hex[0] != '#' || strlen(hex) < 7) {
		*r = *g = *b = 0.133;
		return;
	}

	val = (guint32)strtoul(hex + 1, NULL, 16);
	*r = ((val >> 16) & 0xFF) / 255.0;
	*g = ((val >>  8) & 0xFF) / 255.0;
	*b = ((val      ) & 0xFF) / 255.0;
}

/* --- shm buffer helpers --- */

/**
 * create_shm_file:
 * @size: the desired file size in bytes
 *
 * Creates an anonymous shared memory file descriptor and sizes it
 * to @size bytes.  Uses memfd_create for a truly anonymous fd.
 *
 * Returns: a file descriptor on success, or -1 on error
 */
static gint
create_shm_file(gint size)
{
	gint fd;

	fd = memfd_create("gowlbar-shm", MFD_CLOEXEC);
	if (fd < 0)
		return -1;

	if (ftruncate(fd, size) < 0) {
		close(fd);
		return -1;
	}

	return fd;
}

/**
 * create_buffer:
 *
 * Allocates a wl_shm buffer backed by a memfd-based shared memory
 * pool.  Maps the buffer into our address space for cairo to draw into.
 */
static gboolean
create_buffer(GowlbarOutput *self)
{
	struct wl_shm_pool *pool;

	self->stride = self->width * 4;
	self->shm_size = self->stride * self->height;

	self->shm_fd = create_shm_file(self->shm_size);
	if (self->shm_fd < 0) {
		g_warning("gowlbar: failed to create shm file: %s",
		          g_strerror(errno));
		return FALSE;
	}

	self->shm_data = mmap(NULL, (size_t)self->shm_size,
	                       PROT_READ | PROT_WRITE,
	                       MAP_SHARED, self->shm_fd, 0);
	if (self->shm_data == MAP_FAILED) {
		g_warning("gowlbar: mmap failed: %s", g_strerror(errno));
		close(self->shm_fd);
		self->shm_fd = -1;
		return FALSE;
	}

	pool = wl_shm_create_pool(self->wl_shm, self->shm_fd, self->shm_size);
	self->buffer = wl_shm_pool_create_buffer(
		pool, 0, self->width, self->height,
		self->stride, WL_SHM_FORMAT_ARGB8888);
	wl_shm_pool_destroy(pool);

	return TRUE;
}

/**
 * destroy_buffer:
 *
 * Releases the shm buffer, unmaps memory, and closes the fd.
 */
static void
destroy_buffer(GowlbarOutput *self)
{
	if (self->buffer != NULL) {
		wl_buffer_destroy(self->buffer);
		self->buffer = NULL;
	}
	if (self->shm_data != NULL && self->shm_data != MAP_FAILED) {
		munmap(self->shm_data, (size_t)self->shm_size);
		self->shm_data = NULL;
	}
	if (self->shm_fd >= 0) {
		close(self->shm_fd);
		self->shm_fd = -1;
	}
}

/* --- Layer surface callbacks --- */

/**
 * layer_surface_configure:
 *
 * Called when the compositor assigns a size to the layer surface.
 * Allocates the shm buffer and triggers an initial render.
 */
static void
layer_surface_configure(
	void                          *data,
	struct zwlr_layer_surface_v1  *surface,
	uint32_t                       serial,
	uint32_t                       width,
	uint32_t                       height
){
	GowlbarOutput *self;

	self = (GowlbarOutput *)data;

	zwlr_layer_surface_v1_ack_configure(surface, serial);

	/* Update dimensions if changed */
	if ((gint)width != self->width || (gint)height != self->height) {
		destroy_buffer(self);
		self->width  = (gint)width;
		self->height = (gint)height;

		if (self->width > 0 && self->height > 0) {
			if (!create_buffer(self)) {
				g_warning("gowlbar: buffer creation failed");
				return;
			}
		}
	}

	self->configured = TRUE;

	/* Render initial frame */
	gowlbar_output_render(self);
}

/**
 * layer_surface_closed:
 *
 * Called when the compositor closes the layer surface.
 */
static void
layer_surface_closed(
	void                          *data,
	struct zwlr_layer_surface_v1  *surface
){
	GowlbarOutput *self;

	self = (GowlbarOutput *)data;
	(void)surface;

	g_debug("gowlbar: layer surface closed on output %s",
	        self->name != NULL ? self->name : "(unknown)");

	/* Clean up the layer surface */
	if (self->layer_surface != NULL) {
		zwlr_layer_surface_v1_destroy(self->layer_surface);
		self->layer_surface = NULL;
	}
	if (self->wl_surface != NULL) {
		wl_surface_destroy(self->wl_surface);
		self->wl_surface = NULL;
	}
	self->configured = FALSE;
}

/* --- GObject lifecycle --- */

static void
gowlbar_output_finalize(GObject *object)
{
	GowlbarOutput *self;

	self = GOWLBAR_OUTPUT(object);

	destroy_buffer(self);

	if (self->layer_surface != NULL)
		zwlr_layer_surface_v1_destroy(self->layer_surface);
	if (self->wl_surface != NULL)
		wl_surface_destroy(self->wl_surface);
	if (self->wl_output != NULL)
		wl_output_destroy(self->wl_output);

	g_free(self->name);

	G_OBJECT_CLASS(gowlbar_output_parent_class)->finalize(object);
}

static void
gowlbar_output_class_init(GowlbarOutputClass *klass)
{
	GObjectClass *object_class;

	object_class = G_OBJECT_CLASS(klass);

	object_class->finalize = gowlbar_output_finalize;
}

static void
gowlbar_output_init(GowlbarOutput *self)
{
	self->wl_output     = NULL;
	self->name          = NULL;
	self->global_name   = 0;
	self->wl_surface    = NULL;
	self->layer_surface = NULL;
	self->wl_shm        = NULL;
	self->buffer        = NULL;
	self->shm_data      = NULL;
	self->shm_size      = 0;
	self->shm_fd        = -1;
	self->width         = 0;
	self->height        = 0;
	self->stride        = 0;
	self->config        = NULL;
	self->widgets       = NULL;
	self->configured    = FALSE;
}

/* --- Public API --- */

/**
 * gowlbar_output_new:
 * @wl_output: the Wayland output to attach to
 * @name: (nullable): human-readable output name, or %NULL
 * @global_name: the registry global name (for removal tracking)
 *
 * Creates a new per-output bar instance.  The surface is not yet
 * created; call gowlbar_output_setup_surface() to create it.
 *
 * Returns: (transfer full): a newly allocated #GowlbarOutput
 */
GowlbarOutput *
gowlbar_output_new(
	struct wl_output *wl_output,
	const gchar      *name,
	guint32           global_name
){
	GowlbarOutput *self;

	self = (GowlbarOutput *)g_object_new(GOWLBAR_TYPE_OUTPUT, NULL);

	self->wl_output   = wl_output;
	self->name        = g_strdup(name);
	self->global_name = global_name;

	return self;
}

/**
 * gowlbar_output_setup_surface:
 * @self: the bar output
 * @compositor: the wl_compositor global
 * @layer_shell: the layer shell global
 * @shm: the wl_shm global
 * @height: desired bar height in pixels
 *
 * Creates a wl_surface, requests a layer surface on the top layer
 * anchored to the top edge with exclusive zone, and listens for
 * the configure event.
 */
void
gowlbar_output_setup_surface(
	GowlbarOutput              *self,
	struct wl_compositor        *compositor,
	struct zwlr_layer_shell_v1  *layer_shell,
	struct wl_shm               *shm,
	gint                         height
){
	g_return_if_fail(GOWLBAR_IS_OUTPUT(self));

	/* Already set up */
	if (self->wl_surface != NULL)
		return;

	self->wl_shm  = shm;
	self->height  = height;

	/* Create the wl_surface */
	self->wl_surface = wl_compositor_create_surface(compositor);

	/* Request a layer surface on the top layer */
	self->layer_surface = zwlr_layer_shell_v1_get_layer_surface(
		layer_shell, self->wl_surface, self->wl_output,
		ZWLR_LAYER_SHELL_V1_LAYER_TOP, "gowlbar");

	/* Anchor to top + left + right for full-width bar at top */
	zwlr_layer_surface_v1_set_anchor(self->layer_surface,
		ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
		ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
		ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);

	/* Set desired size: 0 width = fill available, height = bar_height */
	zwlr_layer_surface_v1_set_size(self->layer_surface, 0,
	                                (guint32)height);

	/* Reserve exclusive zone so windows don't overlap the bar */
	zwlr_layer_surface_v1_set_exclusive_zone(self->layer_surface,
	                                          height);

	/* Listen for configure/close events */
	zwlr_layer_surface_v1_add_listener(self->layer_surface,
	                                    &layer_surface_listener, self);

	/* Commit to trigger the configure callback */
	wl_surface_commit(self->wl_surface);
}

/**
 * gowlbar_output_render:
 * @self: the bar output
 *
 * Renders all widgets into the shm buffer using cairo and pango,
 * then attaches and commits the surface.
 *
 * Render pipeline:
 *   1. Clear background with config background colour
 *   2. Create PangoLayout with configured font
 *   3. Calculate widget positions: fixed-width widgets packed left,
 *      expanding widget (-1 width) fills remaining space,
 *      right-side widgets packed from right
 *   4. Call widget->render() for each widget
 *   5. Attach buffer and commit surface
 */
void
gowlbar_output_render(GowlbarOutput *self)
{
	cairo_surface_t *cairo_surface;
	cairo_t *cr;
	PangoLayout *layout;
	PangoFontDescription *font_desc;
	const gchar *font_str;
	gdouble bg_r, bg_g, bg_b;
	gint padding;
	gint cur_x;
	GList *l;

	g_return_if_fail(GOWLBAR_IS_OUTPUT(self));

	if (!self->configured || self->buffer == NULL ||
	    self->shm_data == NULL)
		return;

	/* Create a cairo image surface over the shm buffer */
	cairo_surface = cairo_image_surface_create_for_data(
		(unsigned char *)self->shm_data,
		CAIRO_FORMAT_ARGB32,
		self->width, self->height, self->stride);
	cr = cairo_create(cairo_surface);

	/* Create PangoLayout for text rendering */
	layout = pango_cairo_create_layout(cr);

	/* Apply font from config */
	if (self->config != NULL) {
		font_str = gowlbar_config_get_font(self->config);
	} else {
		font_str = "monospace 10";
	}
	font_desc = pango_font_description_from_string(font_str);
	pango_layout_set_font_description(layout, font_desc);
	pango_font_description_free(font_desc);

	/* Step 1: Clear background */
	if (self->config != NULL) {
		parse_hex_color(
			gowlbar_config_get_background(self->config),
			&bg_r, &bg_g, &bg_b);
	} else {
		bg_r = bg_g = bg_b = 0.133;  /* #222222 fallback */
	}
	cairo_set_source_rgb(cr, bg_r, bg_g, bg_b);
	cairo_paint(cr);

	/* Step 2: Calculate widget positions and render */
	if (self->widgets != NULL) {
		gint total_fixed_width;
		gint expanding_width;

		padding = 0;
		if (self->config != NULL)
			padding = gowlbar_config_get_padding(self->config);

		/*
		 * First pass: calculate total fixed widths and identify
		 * which widgets want to expand (-1 from get_width).
		 */
		total_fixed_width = 0;

		for (l = self->widgets; l != NULL; l = l->next) {
			GowlbarWidget *widget;
			gint w;

			widget = (GowlbarWidget *)l->data;
			w = gowlbar_widget_get_width(widget, cr, layout);

			if (w >= 0) {
				total_fixed_width += w;
			}
		}

		/* Account for padding between widgets */
		total_fixed_width += padding *
			((gint)g_list_length(self->widgets) - 1);

		/* Space available for the expanding widget */
		expanding_width = self->width - total_fixed_width;
		if (expanding_width < 0)
			expanding_width = 0;

		/*
		 * Second pass: render each widget at its computed position.
		 * Fixed-width widgets are packed left-to-right.
		 * The expanding widget gets the remaining space.
		 */
		cur_x = 0;

		for (l = self->widgets; l != NULL; l = l->next) {
			GowlbarWidget *widget;
			gint w;

			widget = (GowlbarWidget *)l->data;
			w = gowlbar_widget_get_width(widget, cr, layout);

			if (w < 0) {
				/* Expanding widget fills remaining space */
				w = expanding_width;
			}

			gowlbar_widget_render(widget, cr, layout,
			                       cur_x, 0, w, self->height);

			cur_x += w + padding;
		}
	}

	g_object_unref(layout);
	cairo_destroy(cr);
	cairo_surface_destroy(cairo_surface);

	/* Attach the buffer and commit */
	wl_surface_attach(self->wl_surface, self->buffer, 0, 0);
	wl_surface_damage_buffer(self->wl_surface,
	                          0, 0, self->width, self->height);
	wl_surface_commit(self->wl_surface);
}

/**
 * gowlbar_output_get_wl_output:
 * @self: the bar output
 *
 * Returns: (transfer none): the underlying wl_output
 */
struct wl_output *
gowlbar_output_get_wl_output(GowlbarOutput *self)
{
	g_return_val_if_fail(GOWLBAR_IS_OUTPUT(self), NULL);

	return self->wl_output;
}

/**
 * gowlbar_output_get_name:
 * @self: the bar output
 *
 * Returns: (transfer none) (nullable): the output name
 */
const gchar *
gowlbar_output_get_name(GowlbarOutput *self)
{
	g_return_val_if_fail(GOWLBAR_IS_OUTPUT(self), NULL);

	return self->name;
}

/**
 * gowlbar_output_get_global_name:
 * @self: the bar output
 *
 * Returns: the registry global name
 */
guint32
gowlbar_output_get_global_name(GowlbarOutput *self)
{
	g_return_val_if_fail(GOWLBAR_IS_OUTPUT(self), 0);

	return self->global_name;
}

/**
 * gowlbar_output_set_config:
 * @self: the bar output
 * @config: (transfer none): the bar configuration
 *
 * Sets the configuration used for rendering colours and font.
 */
void
gowlbar_output_set_config(GowlbarOutput *self, GowlbarConfig *config)
{
	g_return_if_fail(GOWLBAR_IS_OUTPUT(self));

	self->config = config;
}

/**
 * gowlbar_output_set_widgets:
 * @self: the bar output
 * @widgets: (element-type GowlbarWidget) (transfer none): widget list
 *
 * Sets the list of widgets to render in order from left to right.
 * The output borrows the list pointer and does not take ownership.
 */
void
gowlbar_output_set_widgets(GowlbarOutput *self, GList *widgets)
{
	g_return_if_fail(GOWLBAR_IS_OUTPUT(self));

	self->widgets = widgets;
}
