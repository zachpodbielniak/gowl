/*
 * gowl - GObject Wayland Compositor
 * Copyright (C) 2026  Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * gowl-fx-private.h -- shared between the fx translation units only.
 * Not installed, not for modules: a module gets GowlFxGl as an opaque
 * pointer, which is what keeps GL out of five module files.
 */

#ifndef GOWL_FX_PRIVATE_H
#define GOWL_FX_PRIVATE_H

#include "gowl-fx.h"

#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#include <wlr/render/egl.h>
#include <wlr/render/gles2.h>

G_BEGIN_DECLS

typedef struct {
	EGLDisplay display;
	EGLContext context;
	EGLSurface draw;
	EGLSurface read;
	gboolean   restored;
} GowlFxEglSave;

typedef struct {
	GLuint program;
	GLint  u_tex;
	GLint  u_step;
	GLint  a_pos;
	GLint  a_uv;
} GowlFxSamplerProg;

typedef struct {
	GLuint program;
	GLint  u_color;
	GLint  u_alpha;
	GLint  u_aspect;
	GLint  a_pos;
	GLint  a_uv;
} GowlFxBackdropProg;

typedef struct {
	GLuint program;
	GLint  u_mvp;
	GLint  u_tex;
	GLint  u_tint;
	GLint  u_base;
	GLint  u_texamt;
	GLint  u_blur;
	GLint  u_edge;
	GLint  u_edge_width;
	GLint  u_spec;
	GLint  u_alpha;
	GLint  u_fade;
	GLint  u_corner;
	GLint  a_pos;
	GLint  a_uv;
} GowlFxQuadProg;

struct _GowlFxGl {
	struct wlr_renderer *renderer;   /* borrowed */
	EGLDisplay           display;
	EGLContext           context;

	GowlFxQuadProg       quad;
	GowlFxBackdropProg   backdrop;
	GowlFxSamplerProg    copy_2d;
	GowlFxSamplerProg    copy_ext;   /* program 0 without the extension */
	GowlFxSamplerProg    blur;

	GLuint               scratch_fbo;
	GowlFxTexture        scratch_a;
	GowlFxTexture        scratch_b;
};

struct _GowlFxPass {
	GowlFxGl      *gl;
	GowlFxEglSave  save;
	gint           width;
	gint           height;
};

gboolean gowl_fx_egl_enter (GowlFxGl *self, GowlFxEglSave *save);
void     gowl_fx_egl_leave (GowlFxEglSave *save);
GLuint   gowl_fx_link_program (const gchar *vert_src, const gchar *frag_src);
void     gowl_fx_draw_screen_quad (GLint a_pos, GLint a_uv);
gboolean gowl_fx_texture_alloc (GowlFxTexture *tex, gint width, gint height);

G_END_DECLS

#endif /* GOWL_FX_PRIVATE_H */
