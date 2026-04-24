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

#include "gowl-gl-embed-renderer.h"
#include "gowl-core-private.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/gl.h>
#include <GL/glext.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/render/dmabuf.h>

typedef struct {
	gpointer       egl_display;      /* EGLDisplay */
	gpointer       current_image;    /* EGLImageKHR; destroyed per commit */
	gpointer       client;           /* GowlClient *, unowned */
	guint          texture;          /* GLuint; 0 until first import */
	gboolean       supports_dmabuf;
	gint           tex_w, tex_h;
	gint           target_w, target_h;

	/* Resolved function pointers.  eglGetProcAddress returns NULL
	 * when the extension is absent; we cache TRUE in
	 * `supports_dmabuf` only when every pointer resolved. */
	PFNEGLCREATEIMAGEKHRPROC           egl_create_image;
	PFNEGLDESTROYIMAGEKHRPROC          egl_destroy_image;
	PFNGLEGLIMAGETARGETTEXTURE2DOESPROC gl_image_target_2d;
} GowlGlEmbedRendererPrivate;

G_DEFINE_TYPE_WITH_PRIVATE(GowlGlEmbedRenderer,
                           gowl_gl_embed_renderer,
                           GOWL_TYPE_EMBED_RENDERER)

#define GET_PRIV(self) \
	((GowlGlEmbedRendererPrivate *) \
	 gowl_gl_embed_renderer_get_instance_private(self))

/* ---------------------------------------------------------------
 * Extension probing (on construction)
 * --------------------------------------------------------------- */

static gboolean
probe_egl_extensions(GowlGlEmbedRendererPrivate *priv)
{
	EGLDisplay   dpy = (EGLDisplay)priv->egl_display;
	const char  *exts;

	if (dpy == EGL_NO_DISPLAY)
		return FALSE;

	exts = eglQueryString(dpy, EGL_EXTENSIONS);
	if (exts == NULL)
		return FALSE;
	if (strstr(exts, "EGL_EXT_image_dma_buf_import") == NULL)
		return FALSE;

	priv->egl_create_image = (PFNEGLCREATEIMAGEKHRPROC)
		eglGetProcAddress("eglCreateImageKHR");
	priv->egl_destroy_image = (PFNEGLDESTROYIMAGEKHRPROC)
		eglGetProcAddress("eglDestroyImageKHR");
	priv->gl_image_target_2d = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)
		eglGetProcAddress("glEGLImageTargetTexture2DOES");

	return (priv->egl_create_image != NULL
	        && priv->egl_destroy_image != NULL
	        && priv->gl_image_target_2d != NULL);
}

/* ---------------------------------------------------------------
 * DMA-BUF import: wlr_buffer → EGLImage → GL texture
 * --------------------------------------------------------------- */

static gboolean
import_dmabuf(GowlGlEmbedRendererPrivate *priv,
              const struct wlr_dmabuf_attributes *attrs)
{
	EGLDisplay  dpy = (EGLDisplay)priv->egl_display;
	EGLint      egl_attrs[50];
	EGLint     *a = egl_attrs;
	gint        i;

	*a++ = EGL_WIDTH;                         *a++ = attrs->width;
	*a++ = EGL_HEIGHT;                        *a++ = attrs->height;
	*a++ = EGL_LINUX_DRM_FOURCC_EXT;          *a++ = attrs->format;

	for (i = 0; i < attrs->n_planes && i < 4; i++) {
		static const EGLint fd_enum[]      = {
			EGL_DMA_BUF_PLANE0_FD_EXT,
			EGL_DMA_BUF_PLANE1_FD_EXT,
			EGL_DMA_BUF_PLANE2_FD_EXT,
			EGL_DMA_BUF_PLANE3_FD_EXT,
		};
		static const EGLint offset_enum[]  = {
			EGL_DMA_BUF_PLANE0_OFFSET_EXT,
			EGL_DMA_BUF_PLANE1_OFFSET_EXT,
			EGL_DMA_BUF_PLANE2_OFFSET_EXT,
			EGL_DMA_BUF_PLANE3_OFFSET_EXT,
		};
		static const EGLint pitch_enum[]   = {
			EGL_DMA_BUF_PLANE0_PITCH_EXT,
			EGL_DMA_BUF_PLANE1_PITCH_EXT,
			EGL_DMA_BUF_PLANE2_PITCH_EXT,
			EGL_DMA_BUF_PLANE3_PITCH_EXT,
		};

		*a++ = fd_enum[i];     *a++ = attrs->fd[i];
		*a++ = offset_enum[i]; *a++ = attrs->offset[i];
		*a++ = pitch_enum[i];  *a++ = attrs->stride[i];
	}
	*a++ = EGL_NONE;

	/* Release the previous image before overwriting — we cache a
	 * single image at a time.  The texture name is stable across
	 * frames; only the underlying EGLImage changes. */
	if (priv->current_image != NULL) {
		priv->egl_destroy_image(dpy, (EGLImageKHR)priv->current_image);
		priv->current_image = NULL;
	}

	priv->current_image = priv->egl_create_image(
		dpy,
		EGL_NO_CONTEXT,
		EGL_LINUX_DMA_BUF_EXT,
		NULL,
		egl_attrs);
	if (priv->current_image == EGL_NO_IMAGE_KHR) {
		priv->current_image = NULL;
		return FALSE;
	}

	if (priv->texture == 0)
		glGenTextures(1, &priv->texture);

	glBindTexture(GL_TEXTURE_2D, priv->texture);
	priv->gl_image_target_2d(GL_TEXTURE_2D,
	                          (GLeglImageOES)priv->current_image);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glBindTexture(GL_TEXTURE_2D, 0);

	priv->tex_w = attrs->width;
	priv->tex_h = attrs->height;
	return TRUE;
}

/* ---------------------------------------------------------------
 * GowlEmbedRenderer vfunc implementations
 * --------------------------------------------------------------- */

static void
gl_attach(GowlEmbedRenderer *base, gpointer client)
{
	GowlGlEmbedRenderer        *self = GOWL_GL_EMBED_RENDERER(base);
	GowlGlEmbedRendererPrivate *priv = GET_PRIV(self);

	priv->client = client;
}

static void
gl_commit(GowlEmbedRenderer *base)
{
	GowlGlEmbedRenderer          *self = GOWL_GL_EMBED_RENDERER(base);
	GowlGlEmbedRendererPrivate   *priv = GET_PRIV(self);
	GowlClient                   *c;
	struct wlr_surface           *surface;
	struct wlr_buffer            *buf;
	struct wlr_dmabuf_attributes  attrs;

	if (!priv->supports_dmabuf)
		return;

	c = (GowlClient *)priv->client;
	if (c == NULL || c->xdg_toplevel == NULL)
		return;

	surface = c->xdg_toplevel->base->surface;
	if (surface == NULL || surface->buffer == NULL)
		return;

	buf = &surface->buffer->base;
	if (!wlr_buffer_get_dmabuf(buf, &attrs))
		return;  /* SHM-only client; caller should use Cairo */

	(void)import_dmabuf(priv, &attrs);
}

static void
gl_resize(GowlEmbedRenderer *base, gint width, gint height)
{
	GowlGlEmbedRenderer        *self = GOWL_GL_EMBED_RENDERER(base);
	GowlGlEmbedRendererPrivate *priv = GET_PRIV(self);

	priv->target_w = width;
	priv->target_h = height;
}

static void
gl_detach(GowlEmbedRenderer *base)
{
	GowlGlEmbedRenderer        *self = GOWL_GL_EMBED_RENDERER(base);
	GowlGlEmbedRendererPrivate *priv = GET_PRIV(self);
	EGLDisplay                   dpy = (EGLDisplay)priv->egl_display;

	if (priv->current_image != NULL && priv->egl_destroy_image != NULL) {
		priv->egl_destroy_image(dpy, (EGLImageKHR)priv->current_image);
		priv->current_image = NULL;
	}

	priv->client = NULL;
}

static gboolean
gl_supports_dmabuf(GowlEmbedRenderer *base)
{
	GowlGlEmbedRenderer        *self = GOWL_GL_EMBED_RENDERER(base);
	GowlGlEmbedRendererPrivate *priv = GET_PRIV(self);

	return priv->supports_dmabuf;
}

/* ---------------------------------------------------------------
 * GObject lifecycle
 * --------------------------------------------------------------- */

static void
gowl_gl_embed_renderer_dispose(GObject *object)
{
	GowlGlEmbedRenderer        *self = GOWL_GL_EMBED_RENDERER(object);
	GowlGlEmbedRendererPrivate *priv = GET_PRIV(self);

	/* GL texture cleanup would ideally happen with the owning
	 * context current.  If the context was destroyed before us
	 * (GtkGLArea teardown), glDeleteTextures is a no-op anyway.
	 * Release the EGL image first so we don't leak the driver
	 * side. */
	gl_detach(GOWL_EMBED_RENDERER(self));

	if (priv->texture != 0) {
		glDeleteTextures(1, &priv->texture);
		priv->texture = 0;
	}

	G_OBJECT_CLASS(gowl_gl_embed_renderer_parent_class)->dispose(object);
}

static void
gowl_gl_embed_renderer_class_init(GowlGlEmbedRendererClass *klass)
{
	GObjectClass           *object_class = G_OBJECT_CLASS(klass);
	GowlEmbedRendererClass *renderer_class = GOWL_EMBED_RENDERER_CLASS(klass);

	object_class->dispose = gowl_gl_embed_renderer_dispose;

	renderer_class->attach          = gl_attach;
	renderer_class->commit          = gl_commit;
	renderer_class->resize          = gl_resize;
	renderer_class->detach          = gl_detach;
	renderer_class->supports_dmabuf = gl_supports_dmabuf;
}

static void
gowl_gl_embed_renderer_init(GowlGlEmbedRenderer *self)
{
	GowlGlEmbedRendererPrivate *priv = GET_PRIV(self);

	priv->egl_display      = NULL;
	priv->current_image    = NULL;
	priv->client           = NULL;
	priv->texture          = 0;
	priv->supports_dmabuf  = FALSE;
	priv->tex_w = priv->tex_h = 0;
	priv->target_w = priv->target_h = 0;
	priv->egl_create_image  = NULL;
	priv->egl_destroy_image = NULL;
	priv->gl_image_target_2d = NULL;
}

/* ---------------------------------------------------------------
 * Public API
 * --------------------------------------------------------------- */

GowlGlEmbedRenderer *
gowl_gl_embed_renderer_new(gpointer egl_display)
{
	GowlGlEmbedRenderer        *self;
	GowlGlEmbedRendererPrivate *priv;

	self = g_object_new(GOWL_TYPE_GL_EMBED_RENDERER, NULL);
	priv = GET_PRIV(self);

	priv->egl_display     = egl_display;
	priv->supports_dmabuf = probe_egl_extensions(priv);

	return self;
}

guint
gowl_gl_embed_renderer_get_texture(GowlGlEmbedRenderer *self)
{
	g_return_val_if_fail(GOWL_IS_GL_EMBED_RENDERER(self), 0);
	return GET_PRIV(self)->texture;
}

void
gowl_gl_embed_renderer_get_texture_size(GowlGlEmbedRenderer *self,
                                         gint                *width,
                                         gint                *height)
{
	GowlGlEmbedRendererPrivate *priv;

	g_return_if_fail(GOWL_IS_GL_EMBED_RENDERER(self));
	priv = GET_PRIV(self);

	if (width  != NULL) *width  = priv->tex_w;
	if (height != NULL) *height = priv->tex_h;
}
