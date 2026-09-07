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
 * Three things here are worth knowing before changing anything.
 *
 * 1. THERE IS NO DEPTH BUFFER, AND THAT IS DELIBERATE.  A wlr_buffer FBO
 *    has colour only, and attaching depth to a framebuffer wlroots owns
 *    would be reaching into its state.  It is not needed: a convex solid
 *    seen from outside has no two front-facing sides that overlap on
 *    screen, so rejecting back faces on the CPU and drawing backdrop ->
 *    reflection -> cap -> sides is already correct.  Anything added later
 *    that is NOT part of that convex solid has to justify its own place
 *    in that order.
 *
 * 2. Y IS FLIPPED IN THE PROJECTION.  wlroots hands out a buffer whose
 *    first row is the top of the image, while GL's first framebuffer row
 *    is the bottom, so the projection negates Y and world +Y lands at the
 *    top of the picture.  That reverses triangle winding, which is one
 *    more reason back faces are rejected on the CPU rather than by
 *    glCullFace: there is then only one place that has to be right.
 *
 * 3. THE EGL CONTEXT IS BORROWED, NOT OWNED.  Every entry point that
 *    touches GL brackets itself with make-current / restore, because the
 *    current context is global state shared with wlroots' renderer, and
 *    every entry point puts the GL state it changed back.
 */

#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "gowl-cube"

#include "gowl-cube-gl.h"

#include <math.h>
#include <string.h>

#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#include <wlr/render/wlr_renderer.h>
#include <wlr/render/wlr_texture.h>
#include <wlr/render/gles2.h>
#include <wlr/render/egl.h>
#include <wlr/types/wlr_buffer.h>

/* Vertical field of view.  A long lens: a wide one throws the corner of
 * the solid far enough forward that the near side visibly swells, which
 * reads as a fisheye rather than as a desktop. */
#define GOWL_CUBE_FOV_Y (42.0 * G_PI / 180.0)

/* The batch is a handful of quads, so vertex data stays client-side --
 * no VBOs to keep in sync with a context that is not ours. */
#define GOWL_CUBE_GL_MAX_FACES  12

typedef struct {
	GLuint program;
	GLint  u_mvp;
	GLint  u_tex;
	GLint  u_tint;
	GLint  u_base;
	GLint  u_texamt;
	GLint  u_blur;
	GLint  u_edge;
	GLint  u_spec;
	GLint  u_alpha;
	GLint  u_fade;
	GLint  a_pos;
	GLint  a_uv;
} GowlCubeFaceProg;

typedef struct {
	GLuint program;
	GLint  u_tex;
	GLint  a_pos;
	GLint  a_uv;
} GowlCubeCopyProg;

typedef struct {
	GLuint program;
	GLint  u_color;
	GLint  u_alpha;
	GLint  u_aspect;
	GLint  a_pos;
	GLint  a_uv;
} GowlCubeBackProg;

struct _GowlCubeGl {
	struct wlr_renderer *renderer;    /* borrowed */
	EGLDisplay           display;
	EGLContext           context;

	GowlCubeFaceProg     face;
	GowlCubeCopyProg     copy_2d;
	GowlCubeCopyProg     copy_ext;    /* program 0 when OES_EGL_image_external is absent */
	GowlCubeBackProg     backdrop;

	GLuint               scratch_fbo;
};

/* ── EGL bracket ─────────────────────────────────────────────────── */

typedef struct {
	EGLDisplay display;
	EGLContext context;
	EGLSurface draw;
	EGLSurface read;
	gboolean   restored;
} GowlCubeEglSave;

/*
 * The current EGL context is process-global and shared with wlroots.
 * Saving and restoring it is the same contract wlroots' own renderer
 * documents for itself; skipping it turns an unrelated later render pass
 * into a blank screen.
 */
static gboolean
egl_enter(GowlCubeGl *self, GowlCubeEglSave *save)
{
	save->display  = eglGetCurrentDisplay();
	save->context  = eglGetCurrentContext();
	save->draw     = eglGetCurrentSurface(EGL_DRAW);
	save->read     = eglGetCurrentSurface(EGL_READ);
	save->restored = FALSE;

	if (!eglMakeCurrent(self->display, EGL_NO_SURFACE, EGL_NO_SURFACE,
	                    self->context)) {
		g_warning("cube: could not make the renderer's EGL context current");
		return FALSE;
	}
	return TRUE;
}

static void
egl_leave(GowlCubeEglSave *save)
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

static const gchar quad_vert_src[] =
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

static const gchar back_frag_src[] =
	"precision mediump float;\n"
	"uniform vec3  u_color;\n"
	"uniform float u_alpha;\n"
	"uniform float u_aspect;\n"
	"varying vec2 v_uv;\n"
	"void main() {\n"
	/* A flat fill reads as a bug -- as though the compositor lost the
	 * wallpaper.  A pool of light under the solid reads as a stage. */
	"  vec2 p = (v_uv - 0.5) * vec2(u_aspect, 1.0);\n"
	"  float r = length(p);\n"
	"  float g = 1.0 - smoothstep(0.05, 0.85, r);\n"
	"  vec3 c = mix(u_color * 0.30, u_color * 3.4 + 0.022, g);\n"
	"  gl_FragColor = vec4(c * u_alpha, u_alpha);\n"
	"}\n";

static const gchar face_vert_src[] =
	"uniform mat4 u_mvp;\n"
	"attribute vec3 a_pos;\n"
	"attribute vec2 a_uv;\n"
	"varying vec2 v_uv;\n"
	"void main() {\n"
	"  v_uv = a_uv;\n"
	"  gl_Position = u_mvp * vec4(a_pos, 1.0);\n"
	"}\n";

static const gchar face_frag_src[] =
	"precision mediump float;\n"
	"uniform sampler2D u_tex;\n"
	"uniform vec3  u_tint;\n"
	"uniform vec3  u_base;\n"
	"uniform float u_texamt;\n"
	"uniform vec2  u_blur;\n"
	"uniform float u_edge;\n"
	"uniform float u_spec;\n"
	"uniform float u_alpha;\n"
	"uniform float u_fade;\n"
	"varying vec2 v_uv;\n"
	"void main() {\n"
	"  vec3 c;\n"
	"  if (u_texamt > 0.5) {\n"
	/* Five taps along the direction of travel.  A spinning desktop with
	 * razor-sharp text strobes; smearing it along the motion is what the
	 * eye expects and costs four extra samples on two visible sides. */
	"    c  = texture2D(u_tex, v_uv).rgb * 0.40;\n"
	"    c += texture2D(u_tex, clamp(v_uv + u_blur, 0.0, 1.0)).rgb * 0.20;\n"
	"    c += texture2D(u_tex, clamp(v_uv - u_blur, 0.0, 1.0)).rgb * 0.20;\n"
	"    c += texture2D(u_tex, clamp(v_uv + 2.0 * u_blur, 0.0, 1.0)).rgb * 0.10;\n"
	"    c += texture2D(u_tex, clamp(v_uv - 2.0 * u_blur, 0.0, 1.0)).rgb * 0.10;\n"
	"  } else {\n"
	"    c = u_base;\n"
	"  }\n"
	"  c *= u_tint;\n"
	/* A lit bevel along every border.  Without it two adjacent sides of
	 * the same wallpaper melt into one another and the corner disappears,
	 * which is the one line the whole illusion rests on. */
	"  float d = min(min(v_uv.x, 1.0 - v_uv.x), min(v_uv.y, 1.0 - v_uv.y));\n"
	"  float line = 1.0 - smoothstep(0.0, 0.007, d);\n"
	"  c += line * u_edge;\n"
	"  c += u_spec;\n"
	"  float a = u_alpha;\n"
	"  if (u_fade > 0.5)\n"
	"    a *= smoothstep(0.05, 0.95, v_uv.y);\n"
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
		g_warning("cube: shader failed to compile: %s", log);
		glDeleteShader(shader);
		return 0;
	}
	return shader;
}

static GLuint
link_program(const gchar *vert_src, const gchar *frag_src)
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

	/* Attached shaders are reference-counted by the program; deleting the
	 * handles now means the program owns the only reference. */
	glDeleteShader(vert);
	glDeleteShader(frag);

	glGetProgramiv(prog, GL_LINK_STATUS, &ok);
	if (ok != GL_TRUE) {
		gchar log[1024];
		GLsizei len = 0;

		glGetProgramInfoLog(prog, sizeof(log) - 1, &len, log);
		log[len < (GLsizei)sizeof(log) ? len : (GLsizei)sizeof(log) - 1] = '\0';
		g_warning("cube: program failed to link: %s", log);
		glDeleteProgram(prog);
		return 0;
	}
	return prog;
}

static gboolean
copy_prog_init(GowlCubeCopyProg *p, const gchar *frag_src)
{
	p->program = link_program(quad_vert_src, frag_src);
	if (p->program == 0)
		return FALSE;
	p->u_tex = glGetUniformLocation(p->program, "u_tex");
	p->a_pos = glGetAttribLocation(p->program, "a_pos");
	p->a_uv  = glGetAttribLocation(p->program, "a_uv");
	return TRUE;
}

/* ── Small matrix helpers (column-major, as GL wants) ─────────────── */

static void
mat4_identity(gfloat *m)
{
	memset(m, 0, sizeof(gfloat) * 16);
	m[0] = m[5] = m[10] = m[15] = 1.0f;
}

static void
mat4_multiply(gfloat *out, const gfloat *a, const gfloat *b)
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

/*
 * Perspective with Y negated.  See note 2 at the top of the file: the
 * negation is what puts world +Y at the top of a wlroots buffer.
 */
static void
mat4_perspective_flipped(gfloat *m, gdouble fovy, gdouble aspect,
                          gdouble near_z, gdouble far_z)
{
	gdouble f = 1.0 / tan(fovy * 0.5);

	memset(m, 0, sizeof(gfloat) * 16);
	m[0]  = (gfloat)(f / aspect);
	m[5]  = (gfloat)(-f);
	m[10] = (gfloat)((far_z + near_z) / (near_z - far_z));
	m[11] = -1.0f;
	m[14] = (gfloat)(2.0 * far_z * near_z / (near_z - far_z));
}

/*
 * View for a camera orbiting @pitch radians above the equator at @dist.
 *
 * The signs are worth checking against the camera position they imply
 * rather than by eye: solving for the point that maps to the origin of
 * camera space gives (0, dist*sin(pitch), dist*cos(pitch)), i.e. ABOVE
 * the solid for a positive pitch.  The mirror image of this matrix looks
 * equally plausible in source and puts the camera underneath, which shows
 * up as the lid never appearing and the solid riding high in the frame.
 */
static void
mat4_view(gfloat *m, gdouble dist, gdouble pitch)
{
	gdouble c = cos(pitch);
	gdouble s = sin(pitch);

	mat4_identity(m);
	m[5]  = (gfloat)c;
	m[6]  = (gfloat)s;
	m[9]  = (gfloat)(-s);
	m[10] = (gfloat)c;
	m[14] = (gfloat)(-dist);
}

/* ── Lifecycle ───────────────────────────────────────────────────── */

gboolean
gowl_cube_gl_supported(struct wlr_renderer *renderer)
{
	return renderer != NULL && wlr_renderer_is_gles2(renderer);
}

GowlCubeGl *
gowl_cube_gl_new(struct wlr_renderer *renderer)
{
	GowlCubeGl      *self;
	GowlCubeEglSave  save;
	struct wlr_egl  *egl;

	if (!gowl_cube_gl_supported(renderer)) {
		g_message("cube: the renderer is not GLES2, so the cube will sit "
		          "this session out and tag switches stay instant");
		return NULL;
	}

	egl = wlr_gles2_renderer_get_egl(renderer);
	if (egl == NULL)
		return NULL;

	self = g_new0(GowlCubeGl, 1);
	self->renderer = renderer;
	self->display  = wlr_egl_get_display(egl);
	self->context  = wlr_egl_get_context(egl);

	if (!egl_enter(self, &save)) {
		g_free(self);
		return NULL;
	}

	self->face.program = link_program(face_vert_src, face_frag_src);
	self->backdrop.program = link_program(quad_vert_src, back_frag_src);

	if (self->face.program == 0 || self->backdrop.program == 0
	    || !copy_prog_init(&self->copy_2d, copy_frag_2d_src)) {
		egl_leave(&save);
		gowl_cube_gl_free(self);
		return NULL;
	}

	/* External-image sampling is how a dma-buf capture usually arrives.
	 * Its absence is survivable -- such a capture is simply skipped --
	 * so a missing extension must not take the whole module down. */
	if (wlr_gles2_renderer_check_ext(renderer, "GL_OES_EGL_image_external"))
		copy_prog_init(&self->copy_ext, copy_frag_ext_src);

	self->face.u_mvp    = glGetUniformLocation(self->face.program, "u_mvp");
	self->face.u_tex    = glGetUniformLocation(self->face.program, "u_tex");
	self->face.u_tint   = glGetUniformLocation(self->face.program, "u_tint");
	self->face.u_base   = glGetUniformLocation(self->face.program, "u_base");
	self->face.u_texamt = glGetUniformLocation(self->face.program, "u_texamt");
	self->face.u_blur   = glGetUniformLocation(self->face.program, "u_blur");
	self->face.u_edge   = glGetUniformLocation(self->face.program, "u_edge");
	self->face.u_spec   = glGetUniformLocation(self->face.program, "u_spec");
	self->face.u_alpha  = glGetUniformLocation(self->face.program, "u_alpha");
	self->face.u_fade   = glGetUniformLocation(self->face.program, "u_fade");
	self->face.a_pos    = glGetAttribLocation(self->face.program, "a_pos");
	self->face.a_uv     = glGetAttribLocation(self->face.program, "a_uv");

	self->backdrop.u_color  = glGetUniformLocation(self->backdrop.program, "u_color");
	self->backdrop.u_alpha  = glGetUniformLocation(self->backdrop.program, "u_alpha");
	self->backdrop.u_aspect = glGetUniformLocation(self->backdrop.program, "u_aspect");
	self->backdrop.a_pos    = glGetAttribLocation(self->backdrop.program, "a_pos");
	self->backdrop.a_uv     = glGetAttribLocation(self->backdrop.program, "a_uv");

	glGenFramebuffers(1, &self->scratch_fbo);

	egl_leave(&save);
	return self;
}

void
gowl_cube_gl_free(GowlCubeGl *self)
{
	GowlCubeEglSave save;

	if (self == NULL)
		return;

	if (egl_enter(self, &save)) {
		if (self->scratch_fbo != 0)
			glDeleteFramebuffers(1, &self->scratch_fbo);
		if (self->face.program != 0)
			glDeleteProgram(self->face.program);
		if (self->backdrop.program != 0)
			glDeleteProgram(self->backdrop.program);
		if (self->copy_2d.program != 0)
			glDeleteProgram(self->copy_2d.program);
		if (self->copy_ext.program != 0)
			glDeleteProgram(self->copy_ext.program);
		egl_leave(&save);
	}
	g_free(self);
}

/* ── Face storage ────────────────────────────────────────────────── */

void
gowl_cube_gl_drop_face(GowlCubeGl *self, GowlCubeFace *face)
{
	GowlCubeEglSave save;

	if (self == NULL || face == NULL || face->tex == 0)
		return;

	if (egl_enter(self, &save)) {
		GLuint tex = face->tex;

		glDeleteTextures(1, &tex);
		egl_leave(&save);
	}
	face->tex = 0;
	face->width = 0;
	face->height = 0;
}

static void
draw_fullscreen_quad(GLint a_pos, GLint a_uv)
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

gboolean
gowl_cube_gl_store_face(
	GowlCubeGl         *self,
	GowlCubeFace       *face,
	struct wlr_texture *source,
	gint                width,
	gint                height
){
	GowlCubeEglSave save;
	struct wlr_gles2_texture_attribs attribs;
	GowlCubeCopyProg *prog;
	GLuint tex;
	gboolean ok = FALSE;

	if (self == NULL || face == NULL || source == NULL
	    || width <= 0 || height <= 0)
		return FALSE;

	if (!wlr_texture_is_gles2(source))
		return FALSE;

	memset(&attribs, 0, sizeof(attribs));
	wlr_gles2_texture_get_attribs(source, &attribs);

	prog = attribs.target == GL_TEXTURE_EXTERNAL_OES
	           ? &self->copy_ext : &self->copy_2d;
	if (prog->program == 0) {
		g_warning("cube: no shader for this capture's texture target");
		return FALSE;
	}

	if (!egl_enter(self, &save))
		return FALSE;

	/* Reuse the slot's texture when it is already the right size; a
	 * rotation retargeted mid-flight re-captures every face. */
	if (face->tex != 0 && (face->width != width || face->height != height)) {
		tex = face->tex;
		glDeleteTextures(1, &tex);
		face->tex = 0;
	}
	if (face->tex == 0) {
		glGenTextures(1, &tex);
		glBindTexture(GL_TEXTURE_2D, tex);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0,
		             GL_RGBA, GL_UNSIGNED_BYTE, NULL);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glBindTexture(GL_TEXTURE_2D, 0);
		face->tex    = tex;
		face->width  = width;
		face->height = height;
	}

	glBindFramebuffer(GL_FRAMEBUFFER, self->scratch_fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
	                       GL_TEXTURE_2D, face->tex, 0);

	if (glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE) {
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
		draw_fullscreen_quad(prog->a_pos, prog->a_uv);
		glBindTexture(attribs.target, 0);
		glUseProgram(0);
		ok = TRUE;
	} else {
		g_warning("cube: incomplete framebuffer while storing a desktop");
	}

	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
	                       GL_TEXTURE_2D, 0, 0);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	egl_leave(&save);

	if (!ok)
		gowl_cube_gl_drop_face(self, face);
	return ok;
}

/* ── Frame ───────────────────────────────────────────────────────── */

typedef struct {
	gfloat pos[12];   /* four corners, xyz */
	gfloat uv[8];
} GowlCubeQuad;

/*
 * The four corners of the side standing at @phi radians around the axis.
 * Written out rather than composed from a model matrix so the tangent is
 * visible: it is what decides which way motion blur smears.
 */
static void
build_side(GowlCubeQuad *q, gdouble phi, gdouble apothem,
            gdouble half_w, gdouble half_h, gdouble mirror_y)
{
	gdouble sx = sin(phi), cz = cos(phi);
	gdouble cx = apothem * sx, cz_pos = apothem * cz;
	gdouble ux = cos(phi), uz = -sin(phi);
	gdouble top, bottom;
	gint    i;

	/* mirror_y != 0 reflects the side in the plane y = -half_h. */
	top    = mirror_y != 0.0 ?  -2.0 * half_h - half_h : half_h;
	bottom = mirror_y != 0.0 ?  -2.0 * half_h + half_h : -half_h;

	/* top-left, bottom-left, top-right, bottom-right (a strip) */
	q->pos[0]  = (gfloat)(cx - ux * half_w);
	q->pos[1]  = (gfloat)top;
	q->pos[2]  = (gfloat)(cz_pos - uz * half_w);

	q->pos[3]  = (gfloat)(cx - ux * half_w);
	q->pos[4]  = (gfloat)bottom;
	q->pos[5]  = (gfloat)(cz_pos - uz * half_w);

	q->pos[6]  = (gfloat)(cx + ux * half_w);
	q->pos[7]  = (gfloat)top;
	q->pos[8]  = (gfloat)(cz_pos + uz * half_w);

	q->pos[9]  = (gfloat)(cx + ux * half_w);
	q->pos[10] = (gfloat)bottom;
	q->pos[11] = (gfloat)(cz_pos + uz * half_w);

	q->uv[0] = 0.0f; q->uv[1] = 0.0f;
	q->uv[2] = 0.0f; q->uv[3] = 1.0f;
	q->uv[4] = 1.0f; q->uv[5] = 0.0f;
	q->uv[6] = 1.0f; q->uv[7] = 1.0f;

	for (i = 0; i < 12; i++) {
		/* Guard against a NaN slipping into vertex data and taking the
		 * whole draw call out; a dropped frame beats a hung GPU. */
		if (!isfinite(q->pos[i]))
			q->pos[i] = 0.0f;
	}
}

static void
draw_quad(const GowlCubeFaceProg *p, const GowlCubeQuad *q)
{
	glVertexAttribPointer((GLuint)p->a_pos, 3, GL_FLOAT, GL_FALSE, 0, q->pos);
	glEnableVertexAttribArray((GLuint)p->a_pos);
	glVertexAttribPointer((GLuint)p->a_uv, 2, GL_FLOAT, GL_FALSE, 0, q->uv);
	glEnableVertexAttribArray((GLuint)p->a_uv);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	glDisableVertexAttribArray((GLuint)p->a_pos);
	glDisableVertexAttribArray((GLuint)p->a_uv);
}

/* Slot @j's plane angle.  Positive turns the solid so higher tags arrive
 * from the right; the direction flips the whole journey, not the maths. */
static gdouble
slot_phi(const GowlCubeFrame *f, gint j)
{
	return (gdouble)f->dir * ((gdouble)j * f->face_angle - f->rotation);
}

static void
draw_sides(GowlCubeGl *self, const GowlCubeFrame *f, const gfloat *vp,
            gdouble apothem, gdouble half_w, gdouble half_h,
            gdouble cam_y, gdouble cam_z, gboolean reflected)
{
	const GowlCubeFaceProg *p = &self->face;
	gdouble light[3];
	gint    j;
	gfloat  base[3];

	/* A fixed key light above and to the left of the viewer.  Fixed, not
	 * attached to the camera: a highlight that travels across the sides
	 * as they turn is most of what says "this is one solid object". */
	light[0] = -0.42; light[1] = 0.62; light[2] = 0.85;
	{
		gdouble len = sqrt(light[0] * light[0] + light[1] * light[1]
		                   + light[2] * light[2]);
		light[0] /= len; light[1] /= len; light[2] /= len;
	}

	base[0] = f->backdrop[0] * 1.6f + 0.02f;
	base[1] = f->backdrop[1] * 1.6f + 0.02f;
	base[2] = f->backdrop[2] * 1.6f + 0.02f;

	for (j = f->first_slot; j <= f->last_slot; j++) {
		GowlCubeQuad q;
		gdouble phi = slot_phi(f, j);
		gdouble nx = sin(phi), nz = cos(phi);
		gdouble vx, vy, vz, vlen, facing;
		gdouble shade, spec, hx, hy, hz, hlen;
		const GowlCubeFace *face = NULL;
		gfloat tint[3];
		gfloat blur_u;

		/* View vector from the side's centre to the camera. */
		vx = 0.0 - apothem * nx;
		vy = cam_y - 0.0;
		vz = cam_z - apothem * nz;
		vlen = sqrt(vx * vx + vy * vy + vz * vz);
		if (vlen <= 0.0)
			continue;
		vx /= vlen; vy /= vlen; vz /= vlen;

		facing = nx * vx + nz * vz;
		/* Back-face rejection, on the CPU (see note 1 at the top).  The
		 * threshold also drops sides that are exactly edge-on, which
		 * would otherwise draw a one-pixel bright seam. */
		if (facing <= 0.02)
			continue;

		if (f->slot != NULL)
			face = &f->slot[j - f->first_slot];

		shade = (1.0 - f->shading) + f->shading * pow(facing, 0.55);

		/* Half-vector specular.  Narrow and bright: a broad one just
		 * washes the desktop out. */
		hx = light[0] + vx; hy = light[1] + vy; hz = light[2] + vz;
		hlen = sqrt(hx * hx + hy * hy + hz * hz);
		spec = 0.0;
		if (hlen > 0.0) {
			gdouble ndoth = (nx * hx + nz * hz) / hlen;

			if (ndoth > 0.0)
				spec = pow(ndoth, 46.0) * 0.55 * f->bump;
		}

		/* Sides turned away go cool as well as dark.  A purely
		 * multiplicative darkening reads as a dimmer, not as shadow. */
		tint[0] = (gfloat)(shade * (0.90 + 0.10 * shade));
		tint[1] = (gfloat)(shade * (0.94 + 0.06 * shade));
		tint[2] = (gfloat)(shade * (1.10 - 0.10 * shade));

		blur_u = (gfloat)(f->motion_blur * f->speed * 0.010);

		build_side(&q, phi, apothem, half_w, half_h,
		           reflected ? 1.0 : 0.0);

		glUniformMatrix4fv(p->u_mvp, 1, GL_FALSE, vp);
		glUniform3fv(p->u_tint, 1, tint);
		glUniform3fv(p->u_base, 1, base);
		glUniform2f(p->u_blur, blur_u, 0.0f);
		glUniform1f(p->u_edge, (gfloat)(0.20 * f->bump * facing));
		glUniform1f(p->u_spec, (gfloat)spec);
		glUniform1f(p->u_fade, reflected ? 1.0f : 0.0f);
		glUniform1f(p->u_alpha, reflected
		            ? (gfloat)(f->reflection * f->bump) : 1.0f);

		if (face != NULL && face->tex != 0) {
			glUniform1f(p->u_texamt, 1.0f);
			glActiveTexture(GL_TEXTURE0);
			glBindTexture(GL_TEXTURE_2D, face->tex);
			glUniform1i(p->u_tex, 0);
		} else {
			/* No desktop for this slot -- the journey has run out of
			 * tags.  A blank side keeps the solid closed; leaving a hole
			 * would show the backdrop through the middle of the cube. */
			glUniform1f(p->u_texamt, 0.0f);
		}

		draw_quad(p, &q);
	}
	glBindTexture(GL_TEXTURE_2D, 0);
}

static void
draw_cap(GowlCubeGl *self, const GowlCubeFrame *f, const gfloat *vp,
          gdouble circumradius, gdouble half_h, gboolean top)
{
	const GowlCubeFaceProg *p = &self->face;
	gfloat  pos[(GOWL_CUBE_GL_MAX_FACES + 2) * 3];
	gfloat  uv[(GOWL_CUBE_GL_MAX_FACES + 2) * 2];
	gfloat  base[3];
	gint    n = CLAMP(f->faces, 3, GOWL_CUBE_GL_MAX_FACES);
	gint    i, v;
	gdouble y = top ? half_h : -half_h;
	/* The sides sit at slot angles, so the cap's corners must too, or the
	 * lid is rotated off its own box. */
	gdouble phase = slot_phi(f, 0) + f->face_angle * 0.5;

	base[0] = f->backdrop[0] * 2.4f + 0.03f;
	base[1] = f->backdrop[1] * 2.4f + 0.03f;
	base[2] = f->backdrop[2] * 2.4f + 0.04f;

	/* Triangle fan: centre, then the rim, closing on the first corner. */
	pos[0] = 0.0f; pos[1] = (gfloat)y; pos[2] = 0.0f;
	uv[0]  = 0.5f; uv[1]  = 0.5f;
	for (i = 0; i <= n; i++) {
		gdouble a = phase + (gdouble)i * f->face_angle;

		v = i + 1;
		pos[v * 3 + 0] = (gfloat)(circumradius * sin(a));
		pos[v * 3 + 1] = (gfloat)y;
		pos[v * 3 + 2] = (gfloat)(circumradius * cos(a));
		/*
		 * The shader measures its bevel as the distance to the nearest
		 * edge of the unit square, so a radial UV would light only the
		 * four points where the rim touches that square.  Pinning x at
		 * the middle and running y from centre to edge makes the same
		 * expression a uniform rim all the way round.
		 */
		uv[v * 2 + 0] = 0.5f;
		uv[v * 2 + 1] = 1.0f;
	}

	glUniformMatrix4fv(p->u_mvp, 1, GL_FALSE, vp);
	glUniform3f(p->u_tint, 1.0f, 1.0f, 1.0f);
	glUniform3fv(p->u_base, 1, base);
	glUniform1f(p->u_texamt, 0.0f);
	glUniform2f(p->u_blur, 0.0f, 0.0f);
	glUniform1f(p->u_edge, (gfloat)(0.10 * f->bump));
	glUniform1f(p->u_spec, 0.0f);
	glUniform1f(p->u_fade, 0.0f);
	glUniform1f(p->u_alpha, (gfloat)f->bump);

	glVertexAttribPointer((GLuint)p->a_pos, 3, GL_FLOAT, GL_FALSE, 0, pos);
	glEnableVertexAttribArray((GLuint)p->a_pos);
	glVertexAttribPointer((GLuint)p->a_uv, 2, GL_FLOAT, GL_FALSE, 0, uv);
	glEnableVertexAttribArray((GLuint)p->a_uv);
	glDrawArrays(GL_TRIANGLE_FAN, 0, n + 2);
	glDisableVertexAttribArray((GLuint)p->a_pos);
	glDisableVertexAttribArray((GLuint)p->a_uv);
}

gboolean
gowl_cube_gl_render(
	GowlCubeGl          *self,
	struct wlr_buffer   *dst,
	const GowlCubeFrame *frame
){
	GowlCubeEglSave save;
	GLuint  fbo;
	gint    w, h;
	gdouble half_w, half_h, apothem, circumradius;
	gdouble dist_flat, dist, pitch, near_surface;
	gfloat  proj[16], view[16], vp[16];

	if (self == NULL || dst == NULL || frame == NULL)
		return FALSE;

	w = dst->width;
	h = dst->height;
	if (w <= 0 || h <= 0)
		return FALSE;

	half_w = (gdouble)w * 0.5;
	half_h = (gdouble)h * 0.5;

	/*
	 * Apothem: the distance from the axis to the middle of a side.  It is
	 * fixed by the side's width and the number of sides, which is why a
	 * higher face count makes a fatter, shallower drum.
	 */
	apothem      = half_w / tan(G_PI / (gdouble)CLAMP(frame->faces, 3,
	                                                  GOWL_CUBE_GL_MAX_FACES));
	circumradius = half_w / sin(G_PI / (gdouble)CLAMP(frame->faces, 3,
	                                                  GOWL_CUBE_GL_MAX_FACES));

	/*
	 * Camera distance at which the leading side projects to exactly the
	 * viewport.  This is the number that makes the effect seamless: with
	 * the envelope at zero the picture is the desktop, pixel for pixel,
	 * so there is no cut in and no cut out.
	 */
	dist_flat = apothem + half_h / tan(GOWL_CUBE_FOV_Y * 0.5);

	/*
	 * How far the nearest point of the solid has come forward.
	 *
	 * Face on, the nearest surface is a side, at the apothem.  Turned
	 * half a step, it is an EDGE, at the circumradius --- on a cube that
	 * is forty per cent closer to the camera.  Holding the camera at a
	 * fixed distance from the AXIS therefore swells the picture every
	 * time a corner comes round and shrinks it again, which is a pumping
	 * motion nobody asked for and which pushes the solid off the bottom
	 * of the screen at every corner.  Measuring from the nearest surface
	 * instead keeps the apparent size steady through the turn, and since
	 * the two agree exactly when a side is face on, the seamless
	 * endpoints survive it.
	 */
	{
		gdouble step = frame->face_angle > 0.0 ? frame->face_angle : G_PI_2;
		gdouble offset = fmod(frame->rotation + step * 0.5, step);
		gdouble to_face;

		if (offset < 0.0)
			offset += step;
		/* How far the turn is from having a side dead ahead.  Zero means
		 * face on and the nearest surface is the side, at the apothem;
		 * half a step means an edge is ahead, at the circumradius. */
		to_face = fabs(offset - step * 0.5);
		near_surface = circumradius * cos(step * 0.5 - to_face);
	}

	dist  = near_surface + (dist_flat - apothem)
	        * (1.0 + (frame->zoom - 1.0) * frame->bump);
	pitch = frame->pitch_deg * G_PI / 180.0 * frame->bump;

	/*
	 * Then pull back far enough that the solid is actually IN the frame.
	 *
	 * Tilting the camera up over the lid swings the near bottom edge down
	 * the screen, and at the corner --- where that edge is closest --- it
	 * swings clean off the bottom.  Rather than tune the pitch down until
	 * that stops happening, which would cost the lid, solve for the
	 * distance at which the near edge lands inside the margins and take
	 * whichever distance is greater.  The margins are asymmetric on
	 * purpose: the reflection needs the room under the solid, and there
	 * is nothing above it that needs the same.
	 *
	 * Scaled by the envelope so it contributes nothing at either end,
	 * where the picture must still be exactly the flat desktop.
	 */
	{
		gdouble focal  = 1.0 / tan(GOWL_CUBE_FOV_Y * 0.5);
		gdouble c      = cos(pitch);
		gdouble sn     = sin(pitch);
		/* The band under the solid is only worth reserving when there is
		 * a reflection to put in it; with reflections off the framing
		 * tightens up by itself rather than leaving a gap for nothing. */
		gdouble room   = frame->reflection > 0.0 ? 0.78 : 0.92;
		gdouble bottom = c * near_surface - sn * half_h
		                 + focal * (c * half_h + sn * near_surface) / room;
		gdouble top    = c * near_surface + sn * half_h
		                 + focal * (c * half_h - sn * near_surface) / 0.92;
		gdouble fit    = MAX(bottom, top);

		if (fit > dist)
			dist += (fit - dist) * frame->bump;
	}

	if (!egl_enter(self, &save))
		return FALSE;

	fbo = wlr_gles2_renderer_get_buffer_fbo(self->renderer, dst);
	if (fbo == 0) {
		/* No framebuffer for this buffer: nothing has been drawn, so the
		 * caller can still fall back to an instant switch. */
		egl_leave(&save);
		return FALSE;
	}

	mat4_perspective_flipped(proj, GOWL_CUBE_FOV_Y, (gdouble)w / (gdouble)h,
	                          MAX(1.0, (dist - circumradius) * 0.25),
	                          dist + circumradius * 4.0);
	mat4_view(view, dist, pitch);
	mat4_multiply(vp, proj, view);

	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glViewport(0, 0, w, h);
	glDisable(GL_SCISSOR_TEST);
	glDisable(GL_CULL_FACE);
	glDisable(GL_DEPTH_TEST);
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

	/* Opaque clear in the backdrop's own colour: the buffer covers the
	 * whole output, and a transparent one would show the real desktop
	 * underneath the rotation. */
	glClearColor(frame->backdrop[0] * 0.35f, frame->backdrop[1] * 0.35f,
	             frame->backdrop[2] * 0.35f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);

	glEnable(GL_BLEND);
	glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

	if (frame->bump > 0.0) {
		glUseProgram(self->backdrop.program);
		glUniform3fv(self->backdrop.u_color, 1, frame->backdrop);
		glUniform1f(self->backdrop.u_alpha, (gfloat)frame->bump);
		glUniform1f(self->backdrop.u_aspect, (gfloat)w / (gfloat)h);
		draw_fullscreen_quad(self->backdrop.a_pos, self->backdrop.a_uv);
	}

	glUseProgram(self->face.program);

	/*
	 * Painter's order for a convex solid with no depth buffer: whatever
	 * is furthest from the camera first.  The reflection lives below the
	 * floor, the cap is on the far side of the sides from the viewer only
	 * when it is the far cap -- and the far cap is never drawn.
	 */
	if (frame->reflection > 0.0 && frame->bump > 0.0)
		draw_sides(self, frame, vp, apothem, half_w, half_h,
		           dist * sin(pitch), dist * cos(pitch), TRUE);

	/*
	 * Only when the camera has actually cleared the lid.  Without the
	 * check a shallow pitch still draws the cap, and with no depth buffer
	 * it paints a wedge sticking out above the top edge of the sides --
	 * the solid appears to grow a hat.
	 */
	if (frame->caps && frame->bump > 0.0
	    && fabs(dist * sin(pitch)) > half_h)
		draw_cap(self, frame, vp, circumradius, half_h, pitch > 0.0);

	draw_sides(self, frame, vp, apothem, half_w, half_h,
	           dist * sin(pitch), dist * cos(pitch), FALSE);

	/* Put back what wlroots expects to find: its own passes re-bind most
	 * of this, but the bound framebuffer and program are process state
	 * and leaving ours current would redirect somebody else's draw. */
	glUseProgram(0);
	glDisable(GL_BLEND);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glFlush();

	egl_leave(&save);
	return TRUE;
}
