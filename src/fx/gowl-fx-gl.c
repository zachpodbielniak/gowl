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
 * THE EGL CONTEXT IS BORROWED, NOT OWNED.  The current context is process
 * state shared with wlroots' renderer, so every entry point here brackets
 * itself with make-current and restore, and puts back the GL state it
 * changed.  Skipping either turns an unrelated later render pass into a
 * blank screen, some frames after the actual mistake.
 */

#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "gowl-fx"

#include "gowl-fx-private.h"

#include <math.h>
#include <string.h>

#include <wlr/render/wlr_renderer.h>
#include <wlr/render/wlr_texture.h>
#include <wlr/types/wlr_buffer.h>

/* ── EGL bracket ─────────────────────────────────────────────────── */

gboolean
gowl_fx_egl_enter(GowlFxGl *self, GowlFxEglSave *save)
{
	save->display  = eglGetCurrentDisplay();
	save->context  = eglGetCurrentContext();
	save->draw     = eglGetCurrentSurface(EGL_DRAW);
	save->read     = eglGetCurrentSurface(EGL_READ);
	save->restored = FALSE;

	if (!eglMakeCurrent(self->display, EGL_NO_SURFACE, EGL_NO_SURFACE,
	                    self->context)) {
		g_warning("fx: could not make the renderer's EGL context current");
		return FALSE;
	}
	return TRUE;
}

void
gowl_fx_egl_leave(GowlFxEglSave *save)
{
	if (save->restored)
		return;
	save->restored = TRUE;

	if (save->display == EGL_NO_DISPLAY || save->context == EGL_NO_CONTEXT) {
		eglMakeCurrent(eglGetCurrentDisplay(), EGL_NO_SURFACE,
		               EGL_NO_SURFACE, EGL_NO_CONTEXT);
		return;
	}
	eglMakeCurrent(save->display, save->draw, save->read, save->context);
}

/* ── Shaders ─────────────────────────────────────────────────────── */

static const gchar flat_vert_src[] =
	"attribute vec2 a_pos;\n"
	"attribute vec2 a_uv;\n"
	"varying vec2 v_uv;\n"
	"void main() {\n"
	"  v_uv = a_uv;\n"
	"  gl_Position = vec4(a_pos, 0.0, 1.0);\n"
	"}\n";

static const gchar copy_frag_2d_src[] =
	"precision mediump float;\n"
	"uniform sampler2D u_tex;\n"
	"varying vec2 v_uv;\n"
	"void main() { gl_FragColor = vec4(texture2D(u_tex, v_uv).rgb, 1.0); }\n";

static const gchar copy_frag_ext_src[] =
	"#extension GL_OES_EGL_image_external : require\n"
	"precision mediump float;\n"
	"uniform samplerExternalOES u_tex;\n"
	"varying vec2 v_uv;\n"
	"void main() { gl_FragColor = vec4(texture2D(u_tex, v_uv).rgb, 1.0); }\n";

/*
 * One box pass.  Two of these in sequence (horizontal then vertical) are
 * a separable blur; run at a reduced size and repeated, they converge on
 * a gaussian for a fraction of the samples a true gaussian would need.
 */
static const gchar blur_frag_src[] =
	"precision mediump float;\n"
	"uniform sampler2D u_tex;\n"
	"uniform vec2 u_step;\n"
	"varying vec2 v_uv;\n"
	"void main() {\n"
	"  vec3 c = texture2D(u_tex, v_uv).rgb * 0.2270270;\n"
	"  c += texture2D(u_tex, v_uv + u_step * 1.3846153).rgb * 0.3162162;\n"
	"  c += texture2D(u_tex, v_uv - u_step * 1.3846153).rgb * 0.3162162;\n"
	"  c += texture2D(u_tex, v_uv + u_step * 3.2307692).rgb * 0.0702702;\n"
	"  c += texture2D(u_tex, v_uv - u_step * 3.2307692).rgb * 0.0702702;\n"
	"  gl_FragColor = vec4(c, 1.0);\n"
	"}\n";

static const gchar backdrop_frag_src[] =
	"precision mediump float;\n"
	"uniform vec3  u_color;\n"
	"uniform float u_alpha;\n"
	"uniform float u_aspect;\n"
	"varying vec2 v_uv;\n"
	"void main() {\n"
	"  vec2 p = (v_uv - 0.5) * vec2(u_aspect, 1.0);\n"
	"  float r = length(p);\n"
	"  float g = 1.0 - smoothstep(0.05, 0.85, r);\n"
	"  vec3 c = mix(u_color * 0.30, u_color * 3.4 + 0.022, g);\n"
	"  gl_FragColor = vec4(c * u_alpha, u_alpha);\n"
	"}\n";

static const gchar quad_vert_src[] =
	"uniform mat4 u_mvp;\n"
	"attribute vec3 a_pos;\n"
	"attribute vec2 a_uv;\n"
	"varying vec2 v_uv;\n"
	"void main() {\n"
	"  v_uv = a_uv;\n"
	"  gl_Position = u_mvp * vec4(a_pos, 1.0);\n"
	"}\n";

static const gchar quad_frag_src[] =
	"precision mediump float;\n"
	"uniform sampler2D u_tex;\n"
	"uniform vec3  u_tint;\n"
	"uniform vec3  u_base;\n"
	"uniform float u_texamt;\n"
	"uniform vec2  u_blur;\n"
	"uniform float u_edge;\n"
	"uniform float u_edge_width;\n"
	"uniform float u_spec;\n"
	"uniform float u_alpha;\n"
	"uniform float u_fade;\n"
	"uniform float u_corner;\n"
	"varying vec2 v_uv;\n"
	"void main() {\n"
	"  vec3 c;\n"
	"  if (u_texamt > 0.5) {\n"
	/* Five taps along the direction of travel.  A desktop spinning past
	 * with razor-sharp text strobes; smearing it along the motion is what
	 * the eye expects, and it costs four samples on two visible faces. */
	"    c  = texture2D(u_tex, v_uv).rgb * 0.40;\n"
	"    c += texture2D(u_tex, clamp(v_uv + u_blur, 0.0, 1.0)).rgb * 0.20;\n"
	"    c += texture2D(u_tex, clamp(v_uv - u_blur, 0.0, 1.0)).rgb * 0.20;\n"
	"    c += texture2D(u_tex, clamp(v_uv + 2.0 * u_blur, 0.0, 1.0)).rgb * 0.10;\n"
	"    c += texture2D(u_tex, clamp(v_uv - 2.0 * u_blur, 0.0, 1.0)).rgb * 0.10;\n"
	"  } else {\n"
	"    c = u_base;\n"
	"  }\n"
	"  c *= u_tint;\n"
	"  float dx = min(v_uv.x, 1.0 - v_uv.x);\n"
	"  float dy = min(v_uv.y, 1.0 - v_uv.y);\n"
	"  float d = min(dx, dy);\n"
	/* A lit bevel along every border.  Without it two adjacent faces of
	 * the same wallpaper melt into one another and the corner disappears,
	 * which is the one line the whole illusion rests on. */
	"  c += (1.0 - smoothstep(0.0, u_edge_width, d)) * u_edge;\n"
	"  c += u_spec;\n"
	"  float a = u_alpha;\n"
	"  if (u_fade > 0.5)\n"
	"    a *= smoothstep(0.05, 0.95, v_uv.y);\n"
	"  if (u_corner > 0.0) {\n"
	/* Rounded corners as a distance to the inset rectangle, so the
	 * antialiasing is a smoothstep rather than a stair. */
	"    vec2 q = abs(v_uv - 0.5) - (0.5 - u_corner);\n"
	"    float rd = length(max(q, 0.0)) - u_corner;\n"
	"    a *= 1.0 - smoothstep(-0.002, 0.002, rd);\n"
	"  }\n"
	"  gl_FragColor = vec4(c * a, a);\n"
	"}\n";

static GLuint
compile_shader(GLenum type, const gchar *src)
{
	GLuint shader;
	GLint  ok = GL_FALSE;

	shader = glCreateShader(type);
	if (shader == 0)
		return 0;

	glShaderSource(shader, 1, &src, NULL);
	glCompileShader(shader);
	glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
	if (ok != GL_TRUE) {
		gchar log[1024];
		GLsizei len = 0;

		glGetShaderInfoLog(shader, sizeof(log) - 1, &len, log);
		log[len < (GLsizei)sizeof(log) ? len : (GLsizei)sizeof(log) - 1] = '\0';
		g_warning("fx: shader failed to compile: %s", log);
		glDeleteShader(shader);
		return 0;
	}
	return shader;
}

GLuint
gowl_fx_link_program(const gchar *vert_src, const gchar *frag_src)
{
	GLuint vert, frag, prog;
	GLint  ok = GL_FALSE;

	vert = compile_shader(GL_VERTEX_SHADER, vert_src);
	if (vert == 0)
		return 0;

	frag = compile_shader(GL_FRAGMENT_SHADER, frag_src);
	if (frag == 0) {
		glDeleteShader(vert);
		return 0;
	}

	prog = glCreateProgram();
	glAttachShader(prog, vert);
	glAttachShader(prog, frag);
	glLinkProgram(prog);

	/* Attached shaders are reference-counted by the program, so deleting
	 * the handles now leaves the program owning the only reference. */
	glDeleteShader(vert);
	glDeleteShader(frag);

	glGetProgramiv(prog, GL_LINK_STATUS, &ok);
	if (ok != GL_TRUE) {
		gchar log[1024];
		GLsizei len = 0;

		glGetProgramInfoLog(prog, sizeof(log) - 1, &len, log);
		log[len < (GLsizei)sizeof(log) ? len : (GLsizei)sizeof(log) - 1] = '\0';
		g_warning("fx: program failed to link: %s", log);
		glDeleteProgram(prog);
		return 0;
	}
	return prog;
}

static gboolean
sampler_prog_init(GowlFxSamplerProg *p, const gchar *frag_src)
{
	p->program = gowl_fx_link_program(flat_vert_src, frag_src);
	if (p->program == 0)
		return FALSE;
	p->u_tex  = glGetUniformLocation(p->program, "u_tex");
	p->u_step = glGetUniformLocation(p->program, "u_step");
	p->a_pos  = glGetAttribLocation(p->program, "a_pos");
	p->a_uv   = glGetAttribLocation(p->program, "a_uv");
	return TRUE;
}

/* ── Lifecycle ───────────────────────────────────────────────────── */

gboolean
gowl_fx_gl_supported(struct wlr_renderer *renderer)
{
	return renderer != NULL && wlr_renderer_is_gles2(renderer);
}

GowlFxGl *
gowl_fx_gl_new(struct wlr_renderer *renderer)
{
	GowlFxGl      *self;
	GowlFxEglSave  save;
	struct wlr_egl *egl;

	if (!gowl_fx_gl_supported(renderer)) {
		g_message("fx: the renderer is not GLES2, so visual effects will "
		          "sit this session out");
		return NULL;
	}

	egl = wlr_gles2_renderer_get_egl(renderer);
	if (egl == NULL)
		return NULL;

	self = g_new0(GowlFxGl, 1);
	self->renderer = renderer;
	self->display  = wlr_egl_get_display(egl);
	self->context  = wlr_egl_get_context(egl);

	if (!gowl_fx_egl_enter(self, &save)) {
		g_free(self);
		return NULL;
	}

	self->quad.program = gowl_fx_link_program(quad_vert_src, quad_frag_src);
	self->backdrop.program =
		gowl_fx_link_program(flat_vert_src, backdrop_frag_src);

	if (self->quad.program == 0 || self->backdrop.program == 0
	    || !sampler_prog_init(&self->copy_2d, copy_frag_2d_src)
	    || !sampler_prog_init(&self->blur, blur_frag_src)) {
		gowl_fx_egl_leave(&save);
		gowl_fx_gl_free(self);
		return NULL;
	}

	/* External-image sampling is how a dma-buf capture usually arrives.
	 * Its absence is survivable -- such a capture is skipped -- so a
	 * missing extension must not take the whole layer down. */
	if (wlr_gles2_renderer_check_ext(renderer, "GL_OES_EGL_image_external"))
		sampler_prog_init(&self->copy_ext, copy_frag_ext_src);

	self->quad.u_mvp        = glGetUniformLocation(self->quad.program, "u_mvp");
	self->quad.u_tex        = glGetUniformLocation(self->quad.program, "u_tex");
	self->quad.u_tint       = glGetUniformLocation(self->quad.program, "u_tint");
	self->quad.u_base       = glGetUniformLocation(self->quad.program, "u_base");
	self->quad.u_texamt     = glGetUniformLocation(self->quad.program, "u_texamt");
	self->quad.u_blur       = glGetUniformLocation(self->quad.program, "u_blur");
	self->quad.u_edge       = glGetUniformLocation(self->quad.program, "u_edge");
	self->quad.u_edge_width = glGetUniformLocation(self->quad.program, "u_edge_width");
	self->quad.u_spec       = glGetUniformLocation(self->quad.program, "u_spec");
	self->quad.u_alpha      = glGetUniformLocation(self->quad.program, "u_alpha");
	self->quad.u_fade       = glGetUniformLocation(self->quad.program, "u_fade");
	self->quad.u_corner     = glGetUniformLocation(self->quad.program, "u_corner");
	self->quad.a_pos        = glGetAttribLocation(self->quad.program, "a_pos");
	self->quad.a_uv         = glGetAttribLocation(self->quad.program, "a_uv");

	self->backdrop.u_color  = glGetUniformLocation(self->backdrop.program, "u_color");
	self->backdrop.u_alpha  = glGetUniformLocation(self->backdrop.program, "u_alpha");
	self->backdrop.u_aspect = glGetUniformLocation(self->backdrop.program, "u_aspect");
	self->backdrop.a_pos    = glGetAttribLocation(self->backdrop.program, "a_pos");
	self->backdrop.a_uv     = glGetAttribLocation(self->backdrop.program, "a_uv");

	glGenFramebuffers(1, &self->scratch_fbo);

	gowl_fx_egl_leave(&save);
	return self;
}

void
gowl_fx_gl_free(GowlFxGl *self)
{
	GowlFxEglSave save;

	if (self == NULL)
		return;

	if (gowl_fx_egl_enter(self, &save)) {
		gowl_fx_texture_drop(self, &self->scratch_a);
		gowl_fx_texture_drop(self, &self->scratch_b);
		if (self->scratch_fbo != 0)
			glDeleteFramebuffers(1, &self->scratch_fbo);
		if (self->quad.program != 0)
			glDeleteProgram(self->quad.program);
		if (self->backdrop.program != 0)
			glDeleteProgram(self->backdrop.program);
		if (self->copy_2d.program != 0)
			glDeleteProgram(self->copy_2d.program);
		if (self->copy_ext.program != 0)
			glDeleteProgram(self->copy_ext.program);
		if (self->blur.program != 0)
			glDeleteProgram(self->blur.program);
		gowl_fx_egl_leave(&save);
	}
	g_free(self);
}

/* ── Textures ────────────────────────────────────────────────────── */

void
gowl_fx_draw_screen_quad(GLint a_pos, GLint a_uv)
{
	static const gfloat pos[8] = { -1.0f, -1.0f,  1.0f, -1.0f,
	                               -1.0f,  1.0f,  1.0f,  1.0f };
	static const gfloat uv[8]  = {  0.0f,  0.0f,  1.0f,  0.0f,
	                                0.0f,  1.0f,  1.0f,  1.0f };

	if (a_pos < 0)
		return;

	glVertexAttribPointer((GLuint)a_pos, 2, GL_FLOAT, GL_FALSE, 0, pos);
	glEnableVertexAttribArray((GLuint)a_pos);
	if (a_uv >= 0) {
		glVertexAttribPointer((GLuint)a_uv, 2, GL_FLOAT, GL_FALSE, 0, uv);
		glEnableVertexAttribArray((GLuint)a_uv);
	}
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	glDisableVertexAttribArray((GLuint)a_pos);
	if (a_uv >= 0)
		glDisableVertexAttribArray((GLuint)a_uv);
}

/* Allocate or resize an owned texture.  Caller must hold the context. */
gboolean
gowl_fx_texture_alloc(GowlFxTexture *tex, gint width, gint height)
{
	GLuint name;

	if (width <= 0 || height <= 0)
		return FALSE;

	if (tex->tex != 0 && (tex->width != width || tex->height != height)) {
		name = tex->tex;
		glDeleteTextures(1, &name);
		tex->tex = 0;
	}
	if (tex->tex != 0)
		return TRUE;

	glGenTextures(1, &name);
	glBindTexture(GL_TEXTURE_2D, name);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0,
	             GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glBindTexture(GL_TEXTURE_2D, 0);

	tex->tex    = name;
	tex->width  = width;
	tex->height = height;
	return TRUE;
}

void
gowl_fx_texture_drop(GowlFxGl *self, GowlFxTexture *tex)
{
	GowlFxEglSave save;

	if (self == NULL || tex == NULL || tex->tex == 0)
		return;

	if (gowl_fx_egl_enter(self, &save)) {
		GLuint name = tex->tex;

		glDeleteTextures(1, &name);
		gowl_fx_egl_leave(&save);
	}
	tex->tex    = 0;
	tex->width  = 0;
	tex->height = 0;
}

void
gowl_fx_texture_set_filter(GowlFxGl *self, const GowlFxTexture *tex,
                            gboolean smooth)
{
	GowlFxEglSave save;
	GLint mode = smooth ? GL_LINEAR : GL_NEAREST;

	if (self == NULL || tex == NULL || tex->tex == 0)
		return;

	if (!gowl_fx_egl_enter(self, &save))
		return;
	glBindTexture(GL_TEXTURE_2D, tex->tex);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, mode);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, mode);
	glBindTexture(GL_TEXTURE_2D, 0);
	gowl_fx_egl_leave(&save);
}

gboolean
gowl_fx_texture_store(
	GowlFxGl           *self,
	GowlFxTexture      *dst,
	struct wlr_texture *source,
	gint                width,
	gint                height
){
	GowlFxEglSave save;
	struct wlr_gles2_texture_attribs attribs;
	GowlFxSamplerProg *prog;
	gboolean ok = FALSE;

	if (self == NULL || dst == NULL || source == NULL
	    || width <= 0 || height <= 0)
		return FALSE;

	if (!wlr_texture_is_gles2(source))
		return FALSE;

	memset(&attribs, 0, sizeof(attribs));
	wlr_gles2_texture_get_attribs(source, &attribs);

	prog = attribs.target == GL_TEXTURE_EXTERNAL_OES
	           ? &self->copy_ext : &self->copy_2d;
	if (prog->program == 0) {
		g_warning("fx: no shader for this capture's texture target");
		return FALSE;
	}

	if (!gowl_fx_egl_enter(self, &save))
		return FALSE;

	if (gowl_fx_texture_alloc(dst, width, height)) {
		glBindFramebuffer(GL_FRAMEBUFFER, self->scratch_fbo);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
		                       GL_TEXTURE_2D, dst->tex, 0);

		if (glCheckFramebufferStatus(GL_FRAMEBUFFER)
		    == GL_FRAMEBUFFER_COMPLETE) {
			glViewport(0, 0, width, height);
			glDisable(GL_BLEND);
			glDisable(GL_SCISSOR_TEST);
			glDisable(GL_CULL_FACE);
			glUseProgram(prog->program);
			glActiveTexture(GL_TEXTURE0);
			glBindTexture(attribs.target, attribs.tex);
			glTexParameteri(attribs.target, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
			glTexParameteri(attribs.target, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
			glUniform1i(prog->u_tex, 0);
			gowl_fx_draw_screen_quad(prog->a_pos, prog->a_uv);
			glBindTexture(attribs.target, 0);
			glUseProgram(0);
			ok = TRUE;
		} else {
			g_warning("fx: incomplete framebuffer while storing a capture");
		}

		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
		                       GL_TEXTURE_2D, 0, 0);
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
	}

	gowl_fx_egl_leave(&save);

	if (!ok)
		gowl_fx_texture_drop(self, dst);
	return ok;
}

/* One separable half-pass: src -> dst at dst's size, offset by u_step. */
static gboolean
blur_pass(GowlFxGl *self, GowlFxTexture *dst, const GowlFxTexture *src,
           gfloat step_x, gfloat step_y)
{
	if (!gowl_fx_texture_alloc(dst, dst->width, dst->height))
		return FALSE;

	glBindFramebuffer(GL_FRAMEBUFFER, self->scratch_fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
	                       GL_TEXTURE_2D, dst->tex, 0);
	if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
		return FALSE;

	glViewport(0, 0, dst->width, dst->height);
	glDisable(GL_BLEND);
	glUseProgram(self->blur.program);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, src->tex);
	glUniform1i(self->blur.u_tex, 0);
	glUniform2f(self->blur.u_step, step_x, step_y);
	gowl_fx_draw_screen_quad(self->blur.a_pos, self->blur.a_uv);
	glBindTexture(GL_TEXTURE_2D, 0);
	glUseProgram(0);
	return TRUE;
}

gboolean
gowl_fx_texture_blur(
	GowlFxGl            *self,
	GowlFxTexture       *dst,
	const GowlFxTexture *src,
	gint                 downscale,
	gint                 passes
){
	GowlFxEglSave save;
	gint small_w, small_h, i;
	gboolean ok = TRUE;

	if (self == NULL || dst == NULL || src == NULL || src->tex == 0)
		return FALSE;

	downscale = CLAMP(downscale, 1, 8);
	passes    = CLAMP(passes, 1, 6);

	small_w = MAX(1, src->width / downscale);
	small_h = MAX(1, src->height / downscale);

	if (!gowl_fx_egl_enter(self, &save))
		return FALSE;

	/*
	 * Ping-pong between two scratch textures at the reduced size.  Doing
	 * the work small is the whole trick: a radius that would cost dozens
	 * of taps at full size costs the same five taps here, and the final
	 * upscale to @dst turns the reduced detail into exactly the softness
	 * that was wanted anyway.
	 */
	self->scratch_a.width  = small_w;
	self->scratch_a.height = small_h;
	self->scratch_b.width  = small_w;
	self->scratch_b.height = small_h;

	ok = gowl_fx_texture_alloc(&self->scratch_a, small_w, small_h)
	     && gowl_fx_texture_alloc(&self->scratch_b, small_w, small_h);

	if (ok)
		ok = blur_pass(self, &self->scratch_a, src, 1.0f / (gfloat)small_w, 0.0f);

	for (i = 0; ok && i < passes; i++) {
		ok = blur_pass(self, &self->scratch_b, &self->scratch_a,
		               0.0f, 1.0f / (gfloat)small_h);
		if (ok)
			ok = blur_pass(self, &self->scratch_a, &self->scratch_b,
			               1.0f / (gfloat)small_w, 0.0f);
	}

	if (ok && gowl_fx_texture_alloc(dst, src->width, src->height)) {
		glBindFramebuffer(GL_FRAMEBUFFER, self->scratch_fbo);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
		                       GL_TEXTURE_2D, dst->tex, 0);
		if (glCheckFramebufferStatus(GL_FRAMEBUFFER)
		    == GL_FRAMEBUFFER_COMPLETE) {
			glViewport(0, 0, dst->width, dst->height);
			glDisable(GL_BLEND);
			glUseProgram(self->copy_2d.program);
			glActiveTexture(GL_TEXTURE0);
			glBindTexture(GL_TEXTURE_2D, self->scratch_a.tex);
			glUniform1i(self->copy_2d.u_tex, 0);
			gowl_fx_draw_screen_quad(self->copy_2d.a_pos, self->copy_2d.a_uv);
			glBindTexture(GL_TEXTURE_2D, 0);
			glUseProgram(0);
		} else {
			ok = FALSE;
		}
	} else {
		ok = FALSE;
	}

	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
	                       GL_TEXTURE_2D, 0, 0);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	gowl_fx_egl_leave(&save);
	return ok;
}

/* ── Pass ────────────────────────────────────────────────────────── */

void
gowl_fx_quad_init(GowlFxQuad *quad)
{
	if (quad == NULL)
		return;

	memset(quad, 0, sizeof(*quad));
	quad->tint[0] = quad->tint[1] = quad->tint[2] = 1.0f;
	quad->alpha = 1.0f;
	quad->edge_width = 0.007f;
}

GowlFxPass *
gowl_fx_pass_begin(GowlFxGl *self, struct wlr_buffer *dst)
{
	GowlFxPass *pass;
	GLuint      fbo;

	if (self == NULL || dst == NULL || dst->width <= 0 || dst->height <= 0)
		return NULL;

	pass = g_new0(GowlFxPass, 1);
	pass->gl = self;
	pass->width = dst->width;
	pass->height = dst->height;

	if (!gowl_fx_egl_enter(self, &pass->save)) {
		g_free(pass);
		return NULL;
	}

	fbo = wlr_gles2_renderer_get_buffer_fbo(self->renderer, dst);
	if (fbo == 0) {
		/* No framebuffer for this buffer: nothing has been drawn, so the
		 * caller can still abandon the effect cleanly. */
		gowl_fx_egl_leave(&pass->save);
		g_free(pass);
		return NULL;
	}

	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glViewport(0, 0, pass->width, pass->height);
	glDisable(GL_SCISSOR_TEST);
	glDisable(GL_CULL_FACE);
	glDisable(GL_DEPTH_TEST);
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	glEnable(GL_BLEND);
	glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
	return pass;
}

void
gowl_fx_pass_clear(GowlFxPass *pass, const gfloat *rgba)
{
	if (pass == NULL || rgba == NULL)
		return;
	glClearColor(rgba[0], rgba[1], rgba[2], rgba[3]);
	glClear(GL_COLOR_BUFFER_BIT);
}

void
gowl_fx_pass_backdrop(GowlFxPass *pass, const gfloat *rgb, gfloat alpha)
{
	GowlFxGl *gl;

	if (pass == NULL || rgb == NULL || alpha <= 0.0f)
		return;

	gl = pass->gl;
	glUseProgram(gl->backdrop.program);
	glUniform3fv(gl->backdrop.u_color, 1, rgb);
	glUniform1f(gl->backdrop.u_alpha, alpha);
	glUniform1f(gl->backdrop.u_aspect,
	            (gfloat)pass->width / (gfloat)MAX(1, pass->height));
	gowl_fx_draw_screen_quad(gl->backdrop.a_pos, gl->backdrop.a_uv);
	glUseProgram(0);
}

void
gowl_fx_pass_quad(GowlFxPass *pass, const GowlFxQuad *quad)
{
	/*
	 * The screen-filling fallback, in the same corner order the struct
	 * documents: top-left, bottom-left, top-right, bottom-right.  The
	 * image's TOP is at NDC -1, because that is where a wlroots buffer's
	 * first row lands -- the same reason gowl_fx_mat4_perspective()
	 * negates Y.  Writing it the textbook way here would leave a quad
	 * with the identity matrix upside down relative to one drawn through
	 * a projection, which is the sort of inconsistency that gets found
	 * one effect at a time.
	 */
	static const gfloat screen_pos[12] = {
		-1.0f, -1.0f, 0.0f,   -1.0f,  1.0f, 0.0f,
		 1.0f, -1.0f, 0.0f,    1.0f,  1.0f, 0.0f
	};
	static const gfloat default_uv[8] = {
		0.0f, 0.0f,  0.0f, 1.0f,  1.0f, 0.0f,  1.0f, 1.0f
	};
	gfloat identity[16];
	const GowlFxQuadProg *p;
	const gfloat *mvp, *pos, *uv;

	if (pass == NULL || quad == NULL)
		return;

	p = &pass->gl->quad;
	gowl_fx_mat4_identity(identity);
	mvp = quad->mvp != NULL ? quad->mvp : identity;
	pos = quad->pos != NULL ? quad->pos : screen_pos;
	uv  = quad->uv  != NULL ? quad->uv  : default_uv;

	glUseProgram(p->program);
	glUniformMatrix4fv(p->u_mvp, 1, GL_FALSE, mvp);
	glUniform3fv(p->u_tint, 1, quad->tint);
	glUniform3fv(p->u_base, 1, quad->base);
	glUniform2fv(p->u_blur, 1, quad->blur);
	glUniform1f(p->u_edge, quad->edge);
	glUniform1f(p->u_edge_width, quad->edge_width > 0.0f
	            ? quad->edge_width : 0.007f);
	glUniform1f(p->u_spec, quad->spec);
	glUniform1f(p->u_alpha, quad->alpha);
	glUniform1f(p->u_fade, quad->fade);
	glUniform1f(p->u_corner, quad->corner);

	if (quad->texture != 0) {
		glUniform1f(p->u_texamt, 1.0f);
		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, quad->texture);
		glUniform1i(p->u_tex, 0);
	} else {
		glUniform1f(p->u_texamt, 0.0f);
	}

	if (p->a_pos >= 0) {
		glVertexAttribPointer((GLuint)p->a_pos, 3, GL_FLOAT, GL_FALSE, 0, pos);
		glEnableVertexAttribArray((GLuint)p->a_pos);
	}
	if (p->a_uv >= 0) {
		glVertexAttribPointer((GLuint)p->a_uv, 2, GL_FLOAT, GL_FALSE, 0, uv);
		glEnableVertexAttribArray((GLuint)p->a_uv);
	}
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	if (p->a_pos >= 0)
		glDisableVertexAttribArray((GLuint)p->a_pos);
	if (p->a_uv >= 0)
		glDisableVertexAttribArray((GLuint)p->a_uv);

	glBindTexture(GL_TEXTURE_2D, 0);
	glUseProgram(0);
}

gboolean
gowl_fx_pass_end(GowlFxPass *pass)
{
	if (pass == NULL)
		return FALSE;

	/* Put back what wlroots expects to find.  Its own passes re-bind most
	 * of this, but the bound framebuffer and current program are process
	 * state, and leaving ours current redirects somebody else's draw. */
	glUseProgram(0);
	glDisable(GL_BLEND);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glFlush();

	gowl_fx_egl_leave(&pass->save);
	g_free(pass);
	return TRUE;
}

/* ── Matrices ────────────────────────────────────────────────────── */

void
gowl_fx_mat4_identity(gfloat *m)
{
	memset(m, 0, sizeof(gfloat) * 16);
	m[0] = m[5] = m[10] = m[15] = 1.0f;
}

void
gowl_fx_mat4_multiply(gfloat *out, const gfloat *a, const gfloat *b)
{
	gfloat tmp[16];
	gint   c, r, k;

	for (c = 0; c < 4; c++) {
		for (r = 0; r < 4; r++) {
			gfloat sum = 0.0f;

			for (k = 0; k < 4; k++)
				sum += a[k * 4 + r] * b[c * 4 + k];
			tmp[c * 4 + r] = sum;
		}
	}
	memcpy(out, tmp, sizeof(tmp));
}

void
gowl_fx_mat4_perspective(gfloat *m, gdouble fovy, gdouble aspect,
                          gdouble near_z, gdouble far_z)
{
	gdouble f = 1.0 / tan(fovy * 0.5);

	memset(m, 0, sizeof(gfloat) * 16);
	m[0]  = (gfloat)(f / aspect);
	m[5]  = (gfloat)(-f);            /* Y negated: see the header. */
	m[10] = (gfloat)((far_z + near_z) / (near_z - far_z));
	m[11] = -1.0f;
	m[14] = (gfloat)(2.0 * far_z * near_z / (near_z - far_z));
}

void
gowl_fx_mat4_ortho(gfloat *m, gdouble width, gdouble height)
{
	memset(m, 0, sizeof(gfloat) * 16);
	m[0]  = (gfloat)(2.0 / MAX(1.0, width));
	m[5]  = (gfloat)(2.0 / MAX(1.0, height));   /* +2, not -2: with the */
	m[10] = -1.0f;                               /* origin moved to the  */
	m[12] = -1.0f;                               /* top left below, this */
	m[13] = -1.0f;                               /* is the same flip.    */
	m[15] = 1.0f;
}

void
gowl_fx_mat4_view(gfloat *m, gdouble dist, gdouble pitch)
{
	gdouble c = cos(pitch);
	gdouble s = sin(pitch);

	gowl_fx_mat4_identity(m);
	m[5]  = (gfloat)c;
	m[6]  = (gfloat)s;
	m[9]  = (gfloat)(-s);
	m[10] = (gfloat)c;
	m[14] = (gfloat)(-dist);
}
