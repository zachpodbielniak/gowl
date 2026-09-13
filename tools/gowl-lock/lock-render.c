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
 * What the lock screen looks like: an optional backdrop image scaled to
 * the screen it is on, and a ring with a dot per character typed.
 *
 * Drawn with cairo into a wl_shm buffer, one per output, at the output's
 * device resolution -- a lock screen rendered at the logical size and
 * upscaled looks soft on the same panels a wallpaper would.
 */

#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "gowl-lock"

#include <math.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cairo/cairo.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <glib/gstdio.h>

#include "lock.h"
#include "util/gowl-wallpaper-scale.h"

/* ------------------------------------------------------------------
 * wl_shm buffers
 * ------------------------------------------------------------------ */

typedef struct {
	struct wl_buffer *buffer;
	void             *data;
	gsize             size;
} ShmBuffer;

static void
shm_buffer_release(void *data, struct wl_buffer *buffer)
{
	ShmBuffer *b = (ShmBuffer *)data;

	wl_buffer_destroy(buffer);
	munmap(b->data, b->size);
	g_free(b);
}

static const struct wl_buffer_listener shm_buffer_listener = {
	.release = shm_buffer_release,
};

/*
 * A shared-memory buffer of @w x @h ARGB pixels.
 *
 * The backing file is created with g_file_open_tmp and unlinked at once,
 * rather than memfd_create: this tree is gnu89, and memfd_create needs
 * _GNU_SOURCE defined before every header it might reach.  The fd is
 * what matters and it survives the unlink.
 */
static ShmBuffer *
shm_buffer_new(struct wl_shm *shm, gint w, gint h, gint *stride_out)
{
	ShmBuffer *b;
	struct wl_shm_pool *pool;
	g_autofree gchar *path = NULL;
	gint fd;
	gint stride = w * 4;
	gsize size = (gsize)stride * h;
	void *data;

	fd = g_file_open_tmp("gowl-lock-XXXXXX", &path, NULL);
	if (fd < 0)
		return NULL;
	g_unlink(path);
	if (ftruncate(fd, (off_t)size) != 0) {
		close(fd);
		return NULL;
	}
	data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (data == MAP_FAILED) {
		close(fd);
		return NULL;
	}

	pool = wl_shm_create_pool(shm, fd, (int32_t)size);
	b = g_new0(ShmBuffer, 1);
	b->data = data;
	b->size = size;
	b->buffer = wl_shm_pool_create_buffer(pool, 0, w, h, stride,
	                                      WL_SHM_FORMAT_ARGB8888);
	wl_shm_pool_destroy(pool);
	close(fd);

	wl_buffer_add_listener(b->buffer, &shm_buffer_listener, b);
	if (stride_out != NULL)
		*stride_out = stride;
	return b;
}

/* ------------------------------------------------------------------
 * The backdrop
 * ------------------------------------------------------------------ */

void
gowl_lock_load_image(GowlLock *self)
{
	g_autoptr(GError) error = NULL;

	if (self->image_path == NULL || self->image_path[0] == '\0')
		return;
	self->image = gdk_pixbuf_new_from_file(self->image_path, &error);
	if (self->image == NULL)
		g_warning("could not read %s: %s -- the lock screen falls back "
		          "to a plain colour", self->image_path, error->message);
}

void
gowl_lock_free_image(GowlLock *self)
{
	if (self->image != NULL) {
		g_object_unref((GdkPixbuf *)self->image);
		self->image = NULL;
	}
}

/*
 * A GdkPixbuf as a cairo surface.
 *
 * gdk_cairo_set_source_pixbuf() would do this, but it lives in GDK --
 * that is, in GTK -- and this program links neither.  The conversion
 * itself is small: cairo wants ARGB32 with premultiplied alpha in
 * native byte order, gdk-pixbuf hands over straight RGB or RGBA.
 *
 * Returns: (transfer full) (nullable): a new surface
 */
static cairo_surface_t *
pixbuf_to_surface(GdkPixbuf *pixbuf)
{
	cairo_surface_t *surface;
	const guchar *src;
	guchar *dst;
	gint w, h, src_stride, dst_stride, channels, row, col;
	gboolean has_alpha;

	w = gdk_pixbuf_get_width(pixbuf);
	h = gdk_pixbuf_get_height(pixbuf);
	if (w <= 0 || h <= 0)
		return NULL;

	surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
	if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
		cairo_surface_destroy(surface);
		return NULL;
	}

	src        = gdk_pixbuf_get_pixels(pixbuf);
	src_stride = gdk_pixbuf_get_rowstride(pixbuf);
	channels   = gdk_pixbuf_get_n_channels(pixbuf);
	has_alpha  = gdk_pixbuf_get_has_alpha(pixbuf);
	dst        = cairo_image_surface_get_data(surface);
	dst_stride = cairo_image_surface_get_stride(surface);

	for (row = 0; row < h; row++) {
		const guchar *s = src + row * src_stride;
		guint32 *d = (guint32 *)(dst + row * dst_stride);

		for (col = 0; col < w; col++) {
			guint r = s[col * channels + 0];
			guint g = s[col * channels + 1];
			guint b = s[col * channels + 2];
			guint a = has_alpha ? s[col * channels + 3] : 0xFFu;

			if (a != 0xFFu) {
				r = (r * a + 127) / 255;
				g = (g * a + 127) / 255;
				b = (b * a + 127) / 255;
			}
			d[col] = ((guint32)a << 24) | ((guint32)r << 16)
			       | ((guint32)g << 8)  | (guint32)b;
		}
	}
	cairo_surface_mark_dirty(surface);
	return surface;
}

/*
 * Paint the backdrop on a screen of @w x @h device pixels.
 *
 * The same five modes the wallpaper has, from the same integer maths
 * (util/gowl-wallpaper-scale.c, compiled straight into this program --
 * it is pure arithmetic and pulls in nothing).  Sharing it is the point:
 * a lock screen that crops a picture differently from the desktop
 * underneath looks like a different picture.
 */
static void
draw_backdrop(GowlLock *self, cairo_t *cr, gint w, gint h)
{
	GdkPixbuf *source = (GdkPixbuf *)self->image;
	cairo_surface_t *img;
	gint iw, ih;

	cairo_set_source_rgba(cr, self->bg[0], self->bg[1], self->bg[2],
	                      self->bg[3]);
	cairo_paint(cr);

	if (source == NULL)
		return;

	iw = gdk_pixbuf_get_width(source);
	ih = gdk_pixbuf_get_height(source);
	if (iw <= 0 || ih <= 0)
		return;

	img = pixbuf_to_surface(source);
	if (img == NULL)
		return;

	cairo_save(cr);
	if (g_strcmp0(self->image_mode, "stretch") == 0) {
		cairo_scale(cr, (gdouble)w / iw, (gdouble)h / ih);
	} else if (g_strcmp0(self->image_mode, "center") == 0) {
		cairo_translate(cr, (w - iw) / 2.0, (h - ih) / 2.0);
	} else if (g_strcmp0(self->image_mode, "fit") == 0) {
		gint sw, sh, ox, oy;

		gowl_wallpaper_fit_rect(iw, ih, w, h, &sw, &sh, &ox, &oy);
		cairo_translate(cr, ox, oy);
		cairo_scale(cr, (gdouble)sw / iw, (gdouble)sh / ih);
	} else {
		/* fill, and the fallback for anything unrecognised */
		gint sw, sh, cx, cy;

		gowl_wallpaper_cover_rect(iw, ih, w, h, &sw, &sh, &cx, &cy);
		cairo_translate(cr, -cx, -cy);
		cairo_scale(cr, (gdouble)sw / iw, (gdouble)sh / ih);
	}
	cairo_set_source_surface(cr, img, 0.0, 0.0);
	cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BILINEAR);
	cairo_paint(cr);
	cairo_restore(cr);
	cairo_surface_destroy(img);
}

/* ------------------------------------------------------------------
 * The indicator
 * ------------------------------------------------------------------ */

static const gchar *
phase_text(GowlLock *self)
{
	switch (self->phase) {
	case GOWL_LOCK_AUTHENTICATING:
		return "Checking...";
	case GOWL_LOCK_FAILED:
		return "Wrong password";
	case GOWL_LOCK_TYPING:
	case GOWL_LOCK_IDLE:
	default:
		return "Enter password";
	}
}

/*
 * A ring in the middle of the screen with one dot per character, and a
 * line of text under it.
 *
 * Deliberately the same shape the built-in module draws, so switching
 * the two over is not a change of desktop -- only of which process is
 * holding the password.
 */
static void
draw_indicator(GowlLock *self, cairo_t *cr, gint w, gint h, gdouble scale)
{
	const gdouble radius = 56.0 * scale;
	const gdouble cx = w / 2.0;
	const gdouble cy = h / 2.0;
	const gdouble *ring;
	const gchar *text;
	cairo_text_extents_t ext;
	gsize dots;
	gsize i;

	ring = self->phase == GOWL_LOCK_FAILED ? self->error : self->accent;

	/* A dark disc behind it, so the ring reads over any picture. */
	cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.55);
	cairo_arc(cr, cx, cy, radius, 0.0, 2.0 * G_PI);
	cairo_fill(cr);

	cairo_set_line_width(cr, 4.0 * scale);
	cairo_set_source_rgba(cr, ring[0], ring[1], ring[2], ring[3]);
	cairo_arc(cr, cx, cy, radius, 0.0, 2.0 * G_PI);
	cairo_stroke(cr);

	/* One dot per character, around the ring.  Capped so a long
	 * password does not draw a solid band -- and, more to the point, so
	 * the ring never reveals the exact length of one. */
	dots = self->password_len > 0
		? g_utf8_strlen(self->password, (gssize)self->password_len) : 0;
	if (dots > 12)
		dots = 12;
	for (i = 0; i < dots; i++) {
		gdouble a = (2.0 * G_PI * i) / 12.0 - G_PI / 2.0;

		cairo_set_source_rgba(cr, ring[0], ring[1], ring[2], ring[3]);
		cairo_arc(cr, cx + cos(a) * radius, cy + sin(a) * radius,
		          4.0 * scale, 0.0, 2.0 * G_PI);
		cairo_fill(cr);
	}

	text = phase_text(self);
	cairo_select_font_face(cr, self->font, CAIRO_FONT_SLANT_NORMAL,
	                       CAIRO_FONT_WEIGHT_NORMAL);
	cairo_set_font_size(cr, self->font_size * scale);
	cairo_text_extents(cr, text, &ext);
	if (self->phase == GOWL_LOCK_FAILED)
		cairo_set_source_rgba(cr, self->error[0], self->error[1],
		                      self->error[2], self->error[3]);
	else
		cairo_set_source_rgba(cr, self->fg[0], self->fg[1], self->fg[2],
		                      self->fg[3]);
	cairo_move_to(cr, cx - ext.width / 2.0 - ext.x_bearing,
	              cy + radius + 40.0 * scale);
	cairo_show_text(cr, text);

	/* How many attempts have been refused, when asked for: useful on a
	 * shared machine, and off by default because it is also a small
	 * signal to whoever walks past. */
	if (self->show_failures && self->failures > 0) {
		g_autofree gchar *line =
			g_strdup_printf("%d failed attempt%s", self->failures,
			                self->failures == 1 ? "" : "s");

		cairo_set_font_size(cr, self->font_size * 0.75 * scale);
		cairo_text_extents(cr, line, &ext);
		cairo_move_to(cr, cx - ext.width / 2.0 - ext.x_bearing,
		              cy + radius + 72.0 * scale);
		cairo_show_text(cr, line);
	}
}

/* ------------------------------------------------------------------
 * One screen
 * ------------------------------------------------------------------ */

void
gowl_lock_render_output(GowlLock *self, GowlLockOutput *out)
{
	ShmBuffer *b;
	cairo_surface_t *surface;
	cairo_t *cr;
	gint stride = 0;
	gint pw, ph;

	if (self->shm == NULL || out->surface == NULL)
		return;

	pw = out->width * out->scale;
	ph = out->height * out->scale;
	if (pw <= 0 || ph <= 0)
		return;

	b = shm_buffer_new(self->shm, pw, ph, &stride);
	if (b == NULL) {
		g_warning("no shared memory for a %dx%d lock surface", pw, ph);
		return;
	}

	surface = cairo_image_surface_create_for_data((guchar *)b->data,
		CAIRO_FORMAT_ARGB32, pw, ph, stride);
	cr = cairo_create(surface);

	draw_backdrop(self, cr, pw, ph);
	draw_indicator(self, cr, pw, ph, (gdouble)out->scale);

	cairo_destroy(cr);
	cairo_surface_flush(surface);
	cairo_surface_destroy(surface);

	wl_surface_attach(out->surface, b->buffer, 0, 0);
	wl_surface_damage_buffer(out->surface, 0, 0, pw, ph);
	wl_surface_commit(out->surface);
}
