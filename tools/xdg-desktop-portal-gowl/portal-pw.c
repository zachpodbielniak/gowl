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
 * portal-pw.c - The PipeWire stream a cast is delivered through.
 *
 * An application that asked for a screen cast is handed a PipeWire node
 * id and connects to it; this is the other end of that node.  PipeWire
 * allocates the buffers (as memfds, because we ask for MemFd), and each
 * one is wrapped as a wl_buffer so the compositor's capture writes
 * straight into the memory the application will read.  One copy, done
 * by the compositor, into its final destination.
 *
 * The pacing is the consumer's: a frame is requested only when PipeWire
 * hands back a buffer to fill, so an application that stops reading
 * stops the capture instead of building a queue.
 *
 * PipeWire's loop runs on this process's GLib main loop through its fd,
 * the same way the Wayland connection does, so everything here is
 * single-threaded and no locking is needed between the two halves.
 */

#include "portal-pw.h"

#include <pipewire/pipewire.h>
#include <spa/param/video/format-utils.h>
#include <spa/param/video/raw.h>
#include <spa/utils/result.h>
#include <spa/buffer/meta.h>

#include <glib-unix.h>
#include <string.h>

struct _PortalPw {
	struct pw_loop    *loop;
	struct pw_context *context;
	struct pw_core    *core;
	guint              fd_source;
};

struct _PortalPwStream {
	PortalPw            *pw;
	PortalCapture       *capture_ctx;   /* for wrapping memfds as wl_buffers */
	PortalCaptureStream *capture;
	struct pw_stream    *stream;
	struct spa_hook      listener;

	guint32  width;
	guint32  height;
	guint32  stride;
	guint32  shm_format;
	enum spa_video_format spa_format;
	gboolean have_format;
	gboolean source_is_window;
	gboolean negotiated;

	/* The pw_buffer currently lent to the compositor, and the wl_buffer
	 * wrapping its memory.  One at a time; see the file comment. */
	struct pw_buffer *pending;
	gpointer          pending_wl;
};

/* PipeWire's fd on the GLib main loop, so both halves share a thread. */
static gboolean
on_pw_fd(gint fd, GIOCondition cond, gpointer data)
{
	PortalPw *self = data;

	(void)fd;
	(void)cond;
	pw_loop_iterate(self->loop, 0);
	return G_SOURCE_CONTINUE;
}

PortalPw *
portal_pw_new(GError **error)
{
	PortalPw *self;

	pw_init(NULL, NULL);

	self = g_new0(PortalPw, 1);
	self->loop = pw_loop_new(NULL);
	if (self->loop == NULL) {
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED,
		                    "could not create a PipeWire loop");
		g_free(self);
		return NULL;
	}
	self->context = pw_context_new(self->loop, NULL, 0);
	if (self->context == NULL) {
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED,
		                    "could not create a PipeWire context");
		portal_pw_free(self);
		return NULL;
	}
	self->core = pw_context_connect(self->context, NULL, 0);
	if (self->core == NULL) {
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
		                    "could not connect to PipeWire");
		portal_pw_free(self);
		return NULL;
	}
	self->fd_source = g_unix_fd_add(pw_loop_get_fd(self->loop), G_IO_IN,
	                                on_pw_fd, self);
	return self;
}

void
portal_pw_free(PortalPw *self)
{
	if (self == NULL)
		return;
	if (self->fd_source != 0)
		g_source_remove(self->fd_source);
	if (self->core != NULL)
		pw_core_disconnect(self->core);
	if (self->context != NULL)
		pw_context_destroy(self->context);
	if (self->loop != NULL)
		pw_loop_destroy(self->loop);
	g_free(self);
}

/*
 * A wl_shm format to the SPA video format with the same bytes in memory.
 *
 * The capture session reports wl_shm's enum, not a DRM fourcc: the two
 * agree for every format except the two oldest, where wl_shm uses 0 and
 * 1 rather than the fourccs 'AR24' and 'XR24'.  Getting this wrong
 * costs an hour, because the stream negotiates fine and only the
 * colours are wrong.
 *
 * The names also swap round: wl_shm's ARGB8888 is a little-endian
 * 32-bit word, so in memory the bytes are B, G, R, A -- which SPA calls
 * BGRA.
 */
static enum spa_video_format
spa_format_from_shm(guint32 shm)
{
	switch (shm) {
	case 0u:           /* WL_SHM_FORMAT_ARGB8888 */
	case 0x34325241u:  /* the same thing as a fourcc */
		return SPA_VIDEO_FORMAT_BGRA;
	case 1u:           /* WL_SHM_FORMAT_XRGB8888 */
	case 0x34325258u:
		return SPA_VIDEO_FORMAT_BGRx;
	case 0x34324241u:  /* ABGR8888 */
		return SPA_VIDEO_FORMAT_RGBA;
	case 0x34324258u:  /* XBGR8888 */
		return SPA_VIDEO_FORMAT_RGBx;
	default:
		return SPA_VIDEO_FORMAT_UNKNOWN;
	}
}

/* Ask the compositor for the next frame into whatever buffer PipeWire
 * has ready for us.  No buffer, no capture: the consumer sets the pace. */
static void
request_frame(PortalPwStream *self)
{
	struct pw_buffer *b;

	if (self->pending != NULL || self->stream == NULL || !self->negotiated)
		return;
	b = pw_stream_dequeue_buffer(self->stream);
	if (b == NULL)
		return;
	self->pending = b;
	self->pending_wl = b->user_data;
	portal_capture_stream_submit(self->capture, self->pending_wl);
}

static void
on_capture_ready(PortalCaptureStream *capture, gpointer data)
{
	PortalPwStream *self = data;
	struct spa_buffer *buf;

	(void)capture;
	if (self->pending == NULL)
		return;

	buf = self->pending->buffer;
	buf->datas[0].chunk->offset = 0;
	buf->datas[0].chunk->stride = (int32_t)self->stride;
	buf->datas[0].chunk->size = self->stride * self->height;
	buf->datas[0].chunk->flags = 0;

	pw_stream_queue_buffer(self->stream, self->pending);
	self->pending = NULL;
	self->pending_wl = NULL;

	/* Straight on to the next one: the dequeue fails harmlessly when
	 * the consumer has not returned a buffer yet, and add_buffer /
	 * process wake us again when it does. */
	request_frame(self);
}

static void
on_capture_stopped(PortalCaptureStream *capture, gpointer data)
{
	PortalPwStream *self = data;

	(void)capture;
	g_debug("pw: the capture source went away; ending the stream");
	if (self->stream != NULL)
		pw_stream_set_active(self->stream, false);
}

/* The video format the compositor is offering, as a SPA param. */
static const struct spa_pod *
build_format(PortalPwStream *self, struct spa_pod_builder *b)
{
	struct spa_rectangle size = SPA_RECTANGLE(self->width, self->height);
	struct spa_fraction framerate = SPA_FRACTION(0, 1);
	struct spa_fraction max_framerate = SPA_FRACTION(60, 1);

	return spa_pod_builder_add_object(b,
		SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
		SPA_FORMAT_mediaType,      SPA_POD_Id(SPA_MEDIA_TYPE_video),
		SPA_FORMAT_mediaSubtype,   SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
		SPA_FORMAT_VIDEO_format,   SPA_POD_Id(self->spa_format),
		SPA_FORMAT_VIDEO_size,     SPA_POD_Rectangle(&size),
		/* A variable framerate: frames arrive when the screen changes,
		 * which is the honest description of a screen cast. */
		SPA_FORMAT_VIDEO_framerate, SPA_POD_Fraction(&framerate),
		SPA_FORMAT_VIDEO_maxFramerate, SPA_POD_CHOICE_RANGE_Fraction(
			&max_framerate, &framerate, &max_framerate));
}

static void
on_capture_size(PortalCaptureStream *capture, guint32 width, guint32 height,
                guint32 format, gpointer data)
{
	PortalPwStream *self = data;
	enum spa_video_format spa_fmt;
	guint8 buffer[1024];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof buffer);
	const struct spa_pod *params[1];

	(void)capture;

	/*
	 * This fires during the constructor's wait for the first
	 * constraints, before there is a pw_stream to update -- the
	 * constructor reads the geometry itself once the wait returns.
	 * Only a LATER size event, from a window that was resized, has
	 * anything to tell PipeWire.
	 */
	if (self->stream == NULL) {
		self->width = width;
		self->height = height;
		self->stride = width * 4;
		self->shm_format = format;
		return;
	}

	spa_fmt = spa_format_from_shm(format);
	if (spa_fmt == SPA_VIDEO_FORMAT_UNKNOWN) {
		g_warning("pw: the compositor offered wl_shm format %u, which "
		          "this backend does not know", format);
		return;
	}
	if (self->have_format && self->width == width && self->height == height
	    && self->shm_format == format)
		return;

	self->width = width;
	self->height = height;
	self->stride = width * 4;
	self->shm_format = format;
	self->spa_format = spa_fmt;
	self->have_format = TRUE;

	params[0] = build_format(self, &b);
	pw_stream_update_params(self->stream, params, 1);
}

/* PipeWire settled on a format: tell it what buffers we need. */
static void
on_stream_param_changed(void *data, uint32_t id, const struct spa_pod *param)
{
	PortalPwStream *self = data;
	guint8 buffer[1024];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof buffer);
	const struct spa_pod *params[2];
	struct spa_video_info_raw info;

	if (param == NULL || id != SPA_PARAM_Format)
		return;
	spa_zero(info);
	if (spa_format_video_raw_parse(param, &info) < 0)
		return;

	self->width = info.size.width;
	self->height = info.size.height;
	self->stride = self->width * 4;

	/*
	 * MemFd only, and mappable: every buffer has to become a wl_shm
	 * pool, which needs a file descriptor the compositor can map.  A
	 * DmaBuf would be faster still, but it would also mean matching the
	 * compositor's device and modifiers, and shm is what the capture
	 * session offers first.
	 */
	params[0] = spa_pod_builder_add_object(&b,
		SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
		SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(4, 2, 8),
		SPA_PARAM_BUFFERS_blocks,  SPA_POD_Int(1),
		SPA_PARAM_BUFFERS_size,    SPA_POD_Int((int)(self->stride * self->height)),
		SPA_PARAM_BUFFERS_stride,  SPA_POD_Int((int)self->stride),
		SPA_PARAM_BUFFERS_align,   SPA_POD_Int(16),
		SPA_PARAM_BUFFERS_dataType,
			SPA_POD_CHOICE_FLAGS_Int(1 << SPA_DATA_MemFd));
	params[1] = spa_pod_builder_add_object(&b,
		SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
		SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Header),
		SPA_PARAM_META_size, SPA_POD_Int((int)sizeof(struct spa_meta_header)));

	pw_stream_update_params(self->stream, params, 2);
	self->negotiated = TRUE;
	request_frame(self);
}

/* Each buffer PipeWire allocated becomes a wl_buffer over the same
 * memfd, kept in the pw_buffer's user_data so the frame path is a
 * pointer read rather than a lookup. */
static void
on_stream_add_buffer(void *data, struct pw_buffer *buffer)
{
	PortalPwStream *self = data;
	struct spa_data *d = &buffer->buffer->datas[0];

	if (d->type != SPA_DATA_MemFd || d->fd < 0) {
		g_warning("pw: a buffer arrived that is not a memfd; "
		          "it cannot be a capture target");
		buffer->user_data = NULL;
		return;
	}
	buffer->user_data = portal_capture_shm_buffer(
		self->capture_ctx, (gint)d->fd, d->maxsize, self->width,
		self->height, self->stride, self->shm_format);
	request_frame(self);
}

static void
on_stream_remove_buffer(void *data, struct pw_buffer *buffer)
{
	PortalPwStream *self = data;

	if (self->pending == buffer) {
		self->pending = NULL;
		self->pending_wl = NULL;
	}
	portal_capture_buffer_destroy(buffer->user_data);
	buffer->user_data = NULL;
}

static void
on_stream_state_changed(void *data, enum pw_stream_state old,
                        enum pw_stream_state state, const char *err)
{
	PortalPwStream *self = data;

	(void)old;
	g_debug("pw: stream %s%s%s", pw_stream_state_as_string(state),
	        err != NULL ? ": " : "", err != NULL ? err : "");
	if (state == PW_STREAM_STATE_STREAMING)
		request_frame(self);
}

static const struct pw_stream_events stream_events = {
	PW_VERSION_STREAM_EVENTS,
	.state_changed = on_stream_state_changed,
	.param_changed = on_stream_param_changed,
	.add_buffer    = on_stream_add_buffer,
	.remove_buffer = on_stream_remove_buffer,
};

PortalPwStream *
portal_pw_stream_new(PortalPw *self, PortalCapture *capture_ctx,
                     const PortalCaptureSource *source, gboolean with_cursor,
                     GError **error)
{
	PortalPwStream *stream;
	struct pw_properties *props;
	guint8 buffer[1024];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof buffer);
	const struct spa_pod *params[1];
	guint32 w = 0;
	guint32 h = 0;
	int res;

	g_return_val_if_fail(self != NULL, NULL);
	g_return_val_if_fail(capture_ctx != NULL, NULL);
	g_return_val_if_fail(source != NULL, NULL);

	stream = g_new0(PortalPwStream, 1);
	stream->pw = self;
	stream->capture_ctx = capture_ctx;
	stream->source_is_window = source->is_window;

	/* The capture session first: its constraints are what the PipeWire
	 * stream has to advertise, and they only arrive from the
	 * compositor.  Advertising a guess and correcting it later makes
	 * every consumer renegotiate on the first frame. */
	stream->capture = portal_capture_stream_new(capture_ctx, source,
		with_cursor, on_capture_size, on_capture_ready, on_capture_stopped,
		stream, error);
	if (stream->capture == NULL) {
		g_free(stream);
		return NULL;
	}
	if (!portal_capture_stream_wait_ready(stream->capture, 2000)) {
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT,
			"the compositor never described the capture source");
		portal_capture_stream_free(stream->capture);
		g_free(stream);
		return NULL;
	}
	portal_capture_stream_get_geometry(stream->capture, &w, &h);
	stream->width = w;
	stream->height = h;
	stream->stride = w * 4;
	stream->shm_format = portal_capture_stream_get_format(stream->capture);
	stream->spa_format = spa_format_from_shm(stream->shm_format);
	if (stream->spa_format == SPA_VIDEO_FORMAT_UNKNOWN) {
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
			"the compositor offered wl_shm format %u, which this "
			"backend does not know", stream->shm_format);
		portal_capture_stream_free(stream->capture);
		g_free(stream);
		return NULL;
	}
	stream->have_format = TRUE;

	props = pw_properties_new(
		PW_KEY_MEDIA_CLASS, "Video/Source",
		PW_KEY_MEDIA_ROLE, "Screen",
		PW_KEY_MEDIA_CATEGORY, "Capture",
		PW_KEY_NODE_NAME,
			source->is_window ? "gowl-window" : "gowl-screen",
		PW_KEY_NODE_DESCRIPTION,
			source->title != NULL ? source->title : "gowl capture",
		NULL);
	stream->stream = pw_stream_new(self->core, "gowl-screencast", props);
	if (stream->stream == NULL) {
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED,
		                    "could not create a PipeWire stream");
		portal_capture_stream_free(stream->capture);
		g_free(stream);
		return NULL;
	}
	pw_stream_add_listener(stream->stream, &stream->listener, &stream_events,
	                       stream);

	params[0] = build_format(stream, &b);
	res = pw_stream_connect(stream->stream, PW_DIRECTION_OUTPUT, PW_ID_ANY,
		PW_STREAM_FLAG_DRIVER | PW_STREAM_FLAG_ALLOC_BUFFERS, params, 1);
	if (res < 0) {
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
		            "could not connect the PipeWire stream: %s",
		            spa_strerror(res));
		pw_stream_destroy(stream->stream);
		portal_capture_stream_free(stream->capture);
		g_free(stream);
		return NULL;
	}

	return stream;
}

guint32
portal_pw_stream_node_id(PortalPwStream *stream)
{
	if (stream == NULL || stream->stream == NULL)
		return SPA_ID_INVALID;
	return pw_stream_get_node_id(stream->stream);
}

void
portal_pw_stream_get_size(PortalPwStream *stream, guint32 *width,
                          guint32 *height)
{
	if (width != NULL)
		*width = stream != NULL ? stream->width : 0;
	if (height != NULL)
		*height = stream != NULL ? stream->height : 0;
}

void
portal_pw_stream_free(PortalPwStream *stream)
{
	if (stream == NULL)
		return;
	if (stream->stream != NULL) {
		spa_hook_remove(&stream->listener);
		pw_stream_destroy(stream->stream);
	}
	portal_capture_stream_free(stream->capture);
	g_free(stream);
}
