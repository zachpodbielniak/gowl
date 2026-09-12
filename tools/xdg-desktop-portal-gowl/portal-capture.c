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
 * portal-capture.c - ext-image-copy-capture, as the ScreenCast source.
 *
 * The whole reason this backend exists.  xdg-desktop-portal-wlr casts
 * outputs, because wlr-screencopy addresses outputs; sharing one window
 * in a call means sharing the screen and everything else on it.
 * ext-image-capture-source-v1 makes a foreign-toplevel handle a capture
 * source, so a window is addressable, and gowl already advertises both
 * that and the toplevel list.  What was missing was a portal that used
 * them.
 *
 * The frame loop is deliberately one-at-a-time: attach a buffer, ask
 * for a frame, wait for `ready', hand it up, repeat.  A screen cast is
 * paced by its consumer, and queueing ahead only buys latency.
 */

#include "portal-capture.h"

#include <wayland-client.h>
#include <string.h>
#include <errno.h>

#include "ext-image-capture-source-v1-client-protocol.h"
#include "ext-image-copy-capture-v1-client-protocol.h"
#include "ext-foreign-toplevel-list-v1-client-protocol.h"

#include <glib-unix.h>

struct _PortalCapture {
	struct wl_display  *display;
	struct wl_registry *registry;
	struct wl_shm      *shm;
	guint               fd_source;

	struct ext_image_copy_capture_manager_v1 *copy_manager;
	struct ext_output_image_capture_source_manager_v1 *output_sources;
	struct ext_foreign_toplevel_image_capture_source_manager_v1 *toplevel_sources;
	struct ext_foreign_toplevel_list_v1 *toplevel_list;

	GPtrArray *outputs;    /* PortalOutput* */
	GPtrArray *toplevels;  /* PortalToplevel* */
};

typedef struct {
	PortalCapture     *capture;
	struct wl_output  *output;
	guint32            name;
	gchar             *description;
} PortalOutput;

typedef struct {
	PortalCapture *capture;
	struct ext_foreign_toplevel_handle_v1 *handle;
	gchar         *identifier;
	gchar         *title;
	gchar         *app_id;
	gboolean       closed;
} PortalToplevel;

struct _PortalCaptureStream {
	PortalCapture *capture;
	struct ext_image_capture_source_v1      *source;
	struct ext_image_copy_capture_session_v1 *session;
	struct ext_image_copy_capture_frame_v1   *frame;

	guint32  width;
	guint32  height;
	guint32  format;
	gboolean have_format;
	gboolean stopped;

	PortalCaptureSizeFunc    on_size;
	PortalCaptureReadyFunc   on_ready;
	PortalCaptureStoppedFunc on_stopped;
	gpointer                 user_data;
};

void
portal_capture_source_free(PortalCaptureSource *source)
{
	if (source == NULL)
		return;
	g_free(source->id);
	g_free(source->title);
	g_free(source->app_id);
	g_free(source);
}

/* ---------------------------------------------------------------
 * Outputs
 * --------------------------------------------------------------- */

static void
output_geometry(void *data, struct wl_output *output, int32_t x, int32_t y,
                int32_t pw, int32_t ph, int32_t subpixel, const char *make,
                const char *model, int32_t transform)
{
	PortalOutput *self = data;

	(void)output; (void)x; (void)y; (void)pw; (void)ph; (void)subpixel;
	(void)transform;
	if (self->description == NULL && model != NULL)
		self->description = g_strdup_printf("%s %s",
			make != NULL ? make : "", model);
}

static void
output_mode(void *data, struct wl_output *o, uint32_t f, int32_t w, int32_t h,
            int32_t r)
{
	(void)data; (void)o; (void)f; (void)w; (void)h; (void)r;
}

static void
output_done(void *data, struct wl_output *o)
{
	(void)data; (void)o;
}

static void
output_scale(void *data, struct wl_output *o, int32_t factor)
{
	(void)data; (void)o; (void)factor;
}

static void
output_name(void *data, struct wl_output *o, const char *name)
{
	PortalOutput *self = data;

	(void)o;
	g_free(self->description);
	self->description = g_strdup(name);
}

static void
output_description(void *data, struct wl_output *o, const char *description)
{
	(void)data; (void)o; (void)description;
}

static const struct wl_output_listener output_listener = {
	.geometry    = output_geometry,
	.mode        = output_mode,
	.done        = output_done,
	.scale       = output_scale,
	.name        = output_name,
	.description = output_description,
};

/* ---------------------------------------------------------------
 * Toplevels
 * --------------------------------------------------------------- */

static void
toplevel_closed(void *data, struct ext_foreign_toplevel_handle_v1 *handle)
{
	PortalToplevel *self = data;

	(void)handle;
	/* Left in the list but never offered again: a capture already
	 * running on it gets `stopped' from its own session. */
	self->closed = TRUE;
}

static void
toplevel_done(void *data, struct ext_foreign_toplevel_handle_v1 *handle)
{
	(void)data; (void)handle;
}

static void
toplevel_title(void *data, struct ext_foreign_toplevel_handle_v1 *handle,
               const char *title)
{
	PortalToplevel *self = data;

	(void)handle;
	g_free(self->title);
	self->title = g_strdup(title);
}

static void
toplevel_app_id(void *data, struct ext_foreign_toplevel_handle_v1 *handle,
                const char *app_id)
{
	PortalToplevel *self = data;

	(void)handle;
	g_free(self->app_id);
	self->app_id = g_strdup(app_id);
}

static void
toplevel_identifier(void *data, struct ext_foreign_toplevel_handle_v1 *handle,
                    const char *identifier)
{
	PortalToplevel *self = data;

	(void)handle;
	g_free(self->identifier);
	self->identifier = g_strdup(identifier);
}

static const struct ext_foreign_toplevel_handle_v1_listener toplevel_listener = {
	.closed     = toplevel_closed,
	.done       = toplevel_done,
	.title      = toplevel_title,
	.app_id     = toplevel_app_id,
	.identifier = toplevel_identifier,
};

static void
toplevel_list_toplevel(void *data, struct ext_foreign_toplevel_list_v1 *list,
                       struct ext_foreign_toplevel_handle_v1 *handle)
{
	PortalCapture *self = data;
	PortalToplevel *t;

	(void)list;
	t = g_new0(PortalToplevel, 1);
	t->capture = self;
	t->handle = handle;
	ext_foreign_toplevel_handle_v1_add_listener(handle, &toplevel_listener, t);
	g_ptr_array_add(self->toplevels, t);
}

static void
toplevel_list_finished(void *data, struct ext_foreign_toplevel_list_v1 *list)
{
	(void)data; (void)list;
}

static const struct ext_foreign_toplevel_list_v1_listener toplevel_list_listener = {
	.toplevel = toplevel_list_toplevel,
	.finished = toplevel_list_finished,
};

/* ---------------------------------------------------------------
 * Registry
 * --------------------------------------------------------------- */

static void
registry_global(void *data, struct wl_registry *registry, uint32_t name,
                const char *interface, uint32_t version)
{
	PortalCapture *self = data;

	if (g_strcmp0(interface, wl_shm_interface.name) == 0) {
		self->shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
	} else if (g_strcmp0(interface, wl_output_interface.name) == 0) {
		PortalOutput *o = g_new0(PortalOutput, 1);

		o->capture = self;
		o->name = name;
		o->output = wl_registry_bind(registry, name, &wl_output_interface,
		                             version < 4u ? version : 4u);
		wl_output_add_listener(o->output, &output_listener, o);
		g_ptr_array_add(self->outputs, o);
	} else if (g_strcmp0(interface,
	                     ext_image_copy_capture_manager_v1_interface.name) == 0) {
		self->copy_manager = wl_registry_bind(registry, name,
			&ext_image_copy_capture_manager_v1_interface, 1);
	} else if (g_strcmp0(interface,
		ext_output_image_capture_source_manager_v1_interface.name) == 0) {
		self->output_sources = wl_registry_bind(registry, name,
			&ext_output_image_capture_source_manager_v1_interface, 1);
	} else if (g_strcmp0(interface,
		ext_foreign_toplevel_image_capture_source_manager_v1_interface.name) == 0) {
		self->toplevel_sources = wl_registry_bind(registry, name,
			&ext_foreign_toplevel_image_capture_source_manager_v1_interface, 1);
	} else if (g_strcmp0(interface,
	                     ext_foreign_toplevel_list_v1_interface.name) == 0) {
		self->toplevel_list = wl_registry_bind(registry, name,
			&ext_foreign_toplevel_list_v1_interface, 1);
		ext_foreign_toplevel_list_v1_add_listener(self->toplevel_list,
			&toplevel_list_listener, self);
	}
}

static void
registry_global_remove(void *data, struct wl_registry *registry, uint32_t name)
{
	PortalCapture *self = data;
	guint i;

	(void)registry;
	for (i = 0; i < self->outputs->len; i++) {
		PortalOutput *o = g_ptr_array_index(self->outputs, i);

		if (o->name == name) {
			g_ptr_array_remove_index(self->outputs, i);
			return;
		}
	}
}

static const struct wl_registry_listener registry_listener = {
	.global        = registry_global,
	.global_remove = registry_global_remove,
};

static gboolean
on_wayland_fd(gint fd, GIOCondition cond, gpointer data)
{
	PortalCapture *self = data;

	(void)fd;
	(void)cond;
	if (wl_display_dispatch(self->display) < 0)
		return G_SOURCE_REMOVE;
	wl_display_flush(self->display);
	return G_SOURCE_CONTINUE;
}

/* ---------------------------------------------------------------
 * Lifecycle
 * --------------------------------------------------------------- */

static void
output_free(gpointer data)
{
	PortalOutput *o = data;

	if (o->output != NULL)
		wl_output_destroy(o->output);
	g_free(o->description);
	g_free(o);
}

static void
toplevel_free(gpointer data)
{
	PortalToplevel *t = data;

	if (t->handle != NULL)
		ext_foreign_toplevel_handle_v1_destroy(t->handle);
	g_free(t->identifier);
	g_free(t->title);
	g_free(t->app_id);
	g_free(t);
}

PortalCapture *
portal_capture_new(GError **error)
{
	PortalCapture *self;

	self = g_new0(PortalCapture, 1);
	self->outputs = g_ptr_array_new_with_free_func(output_free);
	self->toplevels = g_ptr_array_new_with_free_func(toplevel_free);

	self->display = wl_display_connect(NULL);
	if (self->display == NULL) {
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
		            "could not connect to the compositor: %s",
		            g_strerror(errno));
		portal_capture_free(self);
		return NULL;
	}
	self->registry = wl_display_get_registry(self->display);
	wl_registry_add_listener(self->registry, &registry_listener, self);
	/* Twice: the first settles the globals, the second the toplevel
	 * list's initial handles and their titles. */
	wl_display_roundtrip(self->display);
	wl_display_roundtrip(self->display);

	self->fd_source = g_unix_fd_add(wl_display_get_fd(self->display),
	                                G_IO_IN, on_wayland_fd, self);
	return self;
}

void
portal_capture_free(PortalCapture *self)
{
	if (self == NULL)
		return;
	if (self->fd_source != 0)
		g_source_remove(self->fd_source);
	g_clear_pointer(&self->toplevels, g_ptr_array_unref);
	g_clear_pointer(&self->outputs, g_ptr_array_unref);
	if (self->toplevel_list != NULL)
		ext_foreign_toplevel_list_v1_destroy(self->toplevel_list);
	if (self->toplevel_sources != NULL)
		ext_foreign_toplevel_image_capture_source_manager_v1_destroy(
			self->toplevel_sources);
	if (self->output_sources != NULL)
		ext_output_image_capture_source_manager_v1_destroy(
			self->output_sources);
	if (self->copy_manager != NULL)
		ext_image_copy_capture_manager_v1_destroy(self->copy_manager);
	if (self->shm != NULL)
		wl_shm_destroy(self->shm);
	if (self->registry != NULL)
		wl_registry_destroy(self->registry);
	if (self->display != NULL)
		wl_display_disconnect(self->display);
	g_free(self);
}

gboolean
portal_capture_available(PortalCapture *self, gboolean *windows)
{
	if (windows != NULL)
		*windows = self != NULL && self->toplevel_sources != NULL
		           && self->toplevel_list != NULL;
	return self != NULL && self->copy_manager != NULL && self->shm != NULL
	       && (self->output_sources != NULL || self->toplevel_sources != NULL);
}

GPtrArray *
portal_capture_list_sources(PortalCapture *self, gboolean want_windows,
                            gboolean want_outputs)
{
	GPtrArray *out;
	guint i;

	out = g_ptr_array_new_with_free_func(
		(GDestroyNotify)portal_capture_source_free);
	if (self == NULL)
		return out;

	/* A roundtrip first: a window opened since the last call is one the
	 * human expects to see in the chooser. */
	wl_display_roundtrip(self->display);

	if (want_outputs && self->output_sources != NULL) {
		for (i = 0; i < self->outputs->len; i++) {
			PortalOutput *o = g_ptr_array_index(self->outputs, i);
			PortalCaptureSource *s = g_new0(PortalCaptureSource, 1);

			s->is_window = FALSE;
			s->id = g_strdup(o->description != NULL
			                 ? o->description : "output");
			s->title = g_strdup_printf("Screen: %s", s->id);
			s->handle = o->output;
			g_ptr_array_add(out, s);
		}
	}
	if (want_windows && self->toplevel_sources != NULL) {
		for (i = 0; i < self->toplevels->len; i++) {
			PortalToplevel *t = g_ptr_array_index(self->toplevels, i);
			PortalCaptureSource *s;

			if (t->closed)
				continue;
			s = g_new0(PortalCaptureSource, 1);
			s->is_window = TRUE;
			s->id = g_strdup(t->identifier != NULL ? t->identifier : "");
			s->app_id = g_strdup(t->app_id != NULL ? t->app_id : "");
			s->title = g_strdup_printf("Window: %s%s%s",
				t->title != NULL && *t->title != '\0' ? t->title : "(untitled)",
				t->app_id != NULL && *t->app_id != '\0' ? " — " : "",
				t->app_id != NULL ? t->app_id : "");
			s->handle = t->handle;
			g_ptr_array_add(out, s);
		}
	}
	return out;
}

/* ---------------------------------------------------------------
 * Frames
 * --------------------------------------------------------------- */

static void
frame_transform(void *data, struct ext_image_copy_capture_frame_v1 *frame,
                uint32_t transform)
{
	(void)data; (void)frame; (void)transform;
}

static void
frame_damage(void *data, struct ext_image_copy_capture_frame_v1 *frame,
             int32_t x, int32_t y, int32_t w, int32_t h)
{
	(void)data; (void)frame; (void)x; (void)y; (void)w; (void)h;
}

static void
frame_presentation_time(void *data,
                        struct ext_image_copy_capture_frame_v1 *frame,
                        uint32_t sec_hi, uint32_t sec_lo, uint32_t nsec)
{
	(void)data; (void)frame; (void)sec_hi; (void)sec_lo; (void)nsec;
}

static void
frame_ready(void *data, struct ext_image_copy_capture_frame_v1 *frame)
{
	PortalCaptureStream *self = data;

	(void)frame;
	g_clear_pointer(&self->frame, ext_image_copy_capture_frame_v1_destroy);
	if (self->on_ready != NULL)
		self->on_ready(self, self->user_data);
}

static void
frame_failed(void *data, struct ext_image_copy_capture_frame_v1 *frame,
             uint32_t reason)
{
	PortalCaptureStream *self = data;

	(void)frame;
	g_clear_pointer(&self->frame, ext_image_copy_capture_frame_v1_destroy);
	/*
	 * A buffer the compositor cannot use means the constraints moved
	 * under us -- the window was resized between the size event and the
	 * submit.  That is not fatal: the session sends the new constraints
	 * and the consumer renegotiates.  Anything else ends the stream.
	 */
	if (reason == EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_BUFFER_CONSTRAINTS) {
		g_debug("capture: buffer constraints changed mid-frame");
		return;
	}
	g_warning("capture: frame failed (reason %u)", reason);
	self->stopped = TRUE;
	if (self->on_stopped != NULL)
		self->on_stopped(self, self->user_data);
}

static const struct ext_image_copy_capture_frame_v1_listener frame_listener = {
	.transform         = frame_transform,
	.damage            = frame_damage,
	.presentation_time = frame_presentation_time,
	.ready             = frame_ready,
	.failed            = frame_failed,
};

/* ---------------------------------------------------------------
 * Sessions
 * --------------------------------------------------------------- */

static void
session_buffer_size(void *data,
                    struct ext_image_copy_capture_session_v1 *session,
                    uint32_t width, uint32_t height)
{
	PortalCaptureStream *self = data;

	(void)session;
	self->width = width;
	self->height = height;
}

static void
session_shm_format(void *data,
                   struct ext_image_copy_capture_session_v1 *session,
                   uint32_t format)
{
	PortalCaptureStream *self = data;

	(void)session;
	/* The first offered format is the compositor's preference, and the
	 * only one this backend asks for: shm is the path that lets a
	 * PipeWire memfd be the capture target with nothing in between. */
	if (!self->have_format) {
		self->format = format;
		self->have_format = TRUE;
	}
}

static void
session_dmabuf_device(void *data,
                      struct ext_image_copy_capture_session_v1 *session,
                      struct wl_array *device)
{
	(void)data; (void)session; (void)device;
}

static void
session_dmabuf_format(void *data,
                      struct ext_image_copy_capture_session_v1 *session,
                      uint32_t format, struct wl_array *modifiers)
{
	(void)data; (void)session; (void)format; (void)modifiers;
}

static void
session_done(void *data, struct ext_image_copy_capture_session_v1 *session)
{
	PortalCaptureStream *self = data;

	(void)session;
	if (self->width == 0 || self->height == 0 || !self->have_format)
		return;
	if (self->on_size != NULL)
		self->on_size(self, self->width, self->height, self->format,
		              self->user_data);
}

static void
session_stopped(void *data, struct ext_image_copy_capture_session_v1 *session)
{
	PortalCaptureStream *self = data;

	(void)session;
	self->stopped = TRUE;
	if (self->on_stopped != NULL)
		self->on_stopped(self, self->user_data);
}

static const struct ext_image_copy_capture_session_v1_listener session_listener = {
	.buffer_size    = session_buffer_size,
	.shm_format     = session_shm_format,
	.dmabuf_device  = session_dmabuf_device,
	.dmabuf_format  = session_dmabuf_format,
	.done           = session_done,
	.stopped        = session_stopped,
};

PortalCaptureStream *
portal_capture_stream_new(PortalCapture             *self,
                          const PortalCaptureSource *source,
                          gboolean                   with_cursor,
                          PortalCaptureSizeFunc      on_size,
                          PortalCaptureReadyFunc     on_ready,
                          PortalCaptureStoppedFunc   on_stopped,
                          gpointer                   user_data,
                          GError                   **error)
{
	PortalCaptureStream *stream;
	guint32 options = 0;

	g_return_val_if_fail(self != NULL, NULL);
	g_return_val_if_fail(source != NULL, NULL);

	if (self->copy_manager == NULL) {
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
			"the compositor offers no image-copy-capture");
		return NULL;
	}

	stream = g_new0(PortalCaptureStream, 1);
	stream->capture = self;
	stream->on_size = on_size;
	stream->on_ready = on_ready;
	stream->on_stopped = on_stopped;
	stream->user_data = user_data;

	if (source->is_window) {
		if (self->toplevel_sources == NULL) {
			g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
				"the compositor cannot capture single windows");
			g_free(stream);
			return NULL;
		}
		stream->source =
			ext_foreign_toplevel_image_capture_source_manager_v1_create_source(
				self->toplevel_sources,
				(struct ext_foreign_toplevel_handle_v1 *)source->handle);
	} else {
		if (self->output_sources == NULL) {
			g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
				"the compositor cannot capture outputs");
			g_free(stream);
			return NULL;
		}
		stream->source =
			ext_output_image_capture_source_manager_v1_create_source(
				self->output_sources, (struct wl_output *)source->handle);
	}

	if (with_cursor)
		options |= EXT_IMAGE_COPY_CAPTURE_MANAGER_V1_OPTIONS_PAINT_CURSORS;
	stream->session = ext_image_copy_capture_manager_v1_create_session(
		self->copy_manager, stream->source, options);
	ext_image_copy_capture_session_v1_add_listener(stream->session,
		&session_listener, stream);
	wl_display_flush(self->display);
	return stream;
}

void
portal_capture_stream_submit(PortalCaptureStream *stream, gpointer buffer)
{
	g_return_if_fail(stream != NULL);

	if (stream->stopped || stream->session == NULL || buffer == NULL)
		return;
	/* One in flight: a second submit before `ready' would be a protocol
	 * error, and there is nothing to gain from it. */
	if (stream->frame != NULL)
		return;

	stream->frame = ext_image_copy_capture_session_v1_create_frame(
		stream->session);
	ext_image_copy_capture_frame_v1_add_listener(stream->frame,
		&frame_listener, stream);
	ext_image_copy_capture_frame_v1_attach_buffer(stream->frame,
		(struct wl_buffer *)buffer);
	ext_image_copy_capture_frame_v1_damage_buffer(stream->frame, 0, 0,
		(int32_t)stream->width, (int32_t)stream->height);
	ext_image_copy_capture_frame_v1_capture(stream->frame);
	wl_display_flush(stream->capture->display);
}

gboolean
portal_capture_stream_wait_ready(PortalCaptureStream *stream, guint timeout_ms)
{
	gint64 end;

	g_return_val_if_fail(stream != NULL, FALSE);

	end = g_get_monotonic_time() + (gint64)timeout_ms * 1000;
	while (!stream->stopped
	       && (stream->width == 0 || !stream->have_format)) {
		if (g_get_monotonic_time() > end)
			return FALSE;
		if (wl_display_roundtrip(stream->capture->display) < 0)
			return FALSE;
	}
	return !stream->stopped && stream->width > 0 && stream->have_format;
}

guint32
portal_capture_stream_get_format(PortalCaptureStream *stream)
{
	return stream != NULL ? stream->format : 0;
}

void
portal_capture_stream_get_geometry(PortalCaptureStream *stream,
                                   guint32 *width, guint32 *height)
{
	if (width != NULL)
		*width = stream != NULL ? stream->width : 0;
	if (height != NULL)
		*height = stream != NULL ? stream->height : 0;
}

gpointer
portal_capture_shm_buffer(PortalCapture *self, gint fd, gsize size,
                          guint32 width, guint32 height, guint32 stride,
                          guint32 format)
{
	struct wl_shm_pool *pool;
	struct wl_buffer *buffer;

	g_return_val_if_fail(self != NULL, NULL);
	g_return_val_if_fail(self->shm != NULL, NULL);

	pool = wl_shm_create_pool(self->shm, fd, (int32_t)size);
	if (pool == NULL)
		return NULL;
	/* @format came from the session's shm_format event, which is
	 * already a wl_shm value -- the fourccs are accepted too, for a
	 * caller that has one, since the two enums agree beyond the first
	 * two entries. */
	if (format == 0x34325241u)       /* DRM_FORMAT_ARGB8888 */
		format = WL_SHM_FORMAT_ARGB8888;
	else if (format == 0x34325258u)  /* DRM_FORMAT_XRGB8888 */
		format = WL_SHM_FORMAT_XRGB8888;
	buffer = wl_shm_pool_create_buffer(pool, 0, (int32_t)width,
		(int32_t)height, (int32_t)stride, format);
	wl_shm_pool_destroy(pool);
	return buffer;
}

void
portal_capture_buffer_destroy(gpointer buffer)
{
	if (buffer != NULL)
		wl_buffer_destroy((struct wl_buffer *)buffer);
}

void
portal_capture_stream_free(PortalCaptureStream *stream)
{
	if (stream == NULL)
		return;
	g_clear_pointer(&stream->frame, ext_image_copy_capture_frame_v1_destroy);
	if (stream->session != NULL)
		ext_image_copy_capture_session_v1_destroy(stream->session);
	if (stream->source != NULL)
		ext_image_capture_source_v1_destroy(stream->source);
	if (stream->capture != NULL)
		wl_display_flush(stream->capture->display);
	g_free(stream);
}
