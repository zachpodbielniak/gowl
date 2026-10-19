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
 * Textured triangles in one draw call.
 *
 * GowlFxQuad is one rectangle per call, which is right for a handful of
 * desktops on a cube or a carousel and wrong for a desktop bent onto a
 * sphere (a few thousand triangles) or broken into a few hundred shards.
 * This is the same idea -- a texture, an MVP, premultiplied output --
 * with the geometry in a client-side array: the caller builds it on the
 * CPU each frame, which is what lets every shard spin on its own axis
 * without a shader per effect.
 *
 * Each vertex carries a SHADE (multiplies the colour: the caller's
 * lighting) and an ALPHA (a shard fading as it flies).  There is still no
 * depth buffer; see gowl_fx_pass_begin().
 */

#include "gowl-fx-private.h"

static const gchar mesh_vert_src[] =
	"uniform mat4 u_mvp;\n"
	"attribute vec3 a_pos;\n"
	"attribute vec2 a_uv;\n"
	"attribute vec2 a_shade;   /* x: shade, y: alpha */\n"
	"varying vec2 v_uv;\n"
	"varying vec2 v_shade;\n"
	"void main() {\n"
	"  v_uv = a_uv;\n"
	"  v_shade = a_shade;\n"
	"  gl_Position = u_mvp * vec4(a_pos, 1.0);\n"
	"}\n";

static const gchar mesh_frag_src[] =
	"precision mediump float;\n"
	"uniform sampler2D u_tex;\n"
	"uniform vec3  u_tint;\n"
	"uniform float u_alpha;\n"
	"varying vec2 v_uv;\n"
	"varying vec2 v_shade;\n"
	"void main() {\n"
	"  vec4 c = texture2D(u_tex, clamp(v_uv, 0.0, 1.0));\n"
	"  float a = clamp(v_shade.y * u_alpha, 0.0, 1.0);\n"
	"  /* Premultiplied, as every other fx pass writes. */\n"
	"  gl_FragColor = vec4(c.rgb * u_tint * v_shade.x * a, a);\n"
	"}\n";

static GowlFxMeshProg *
mesh_prog(GowlFxGl *self)
{
	GowlFxMeshProg *p = &self->mesh;

	if (self->mesh_tried)
		return p->program != 0 ? p : NULL;
	self->mesh_tried = TRUE;

	p->program = gowl_fx_link_program(mesh_vert_src, mesh_frag_src);
	if (p->program == 0) {
		g_warning("fx: the mesh shader did not build; mesh transitions "
		          "will switch instantly");
		return NULL;
	}
	p->u_mvp   = glGetUniformLocation(p->program, "u_mvp");
	p->u_tex   = glGetUniformLocation(p->program, "u_tex");
	p->u_tint  = glGetUniformLocation(p->program, "u_tint");
	p->u_alpha = glGetUniformLocation(p->program, "u_alpha");
	p->a_pos   = glGetAttribLocation(p->program, "a_pos");
	p->a_uv    = glGetAttribLocation(p->program, "a_uv");
	p->a_shade = glGetAttribLocation(p->program, "a_shade");
	return p;
}

gboolean
gowl_fx_pass_mesh(GowlFxPass *pass, const GowlFxMesh *mesh)
{
	GowlFxMeshProg *p;
	GLsizei stride = (GLsizei)(GOWL_FX_MESH_STRIDE * sizeof(gfloat));

	g_return_val_if_fail(pass != NULL, FALSE);
	g_return_val_if_fail(mesh != NULL, FALSE);

	if (mesh->verts == NULL || mesh->n_verts < 3 || mesh->texture == 0
	    || mesh->mvp == NULL)
		return FALSE;
	p = mesh_prog(pass->gl);
	if (p == NULL || p->a_pos < 0)
		return FALSE;

	glUseProgram(p->program);
	glUniformMatrix4fv(p->u_mvp, 1, GL_FALSE, mesh->mvp);
	glUniform3f(p->u_tint, mesh->tint[0], mesh->tint[1], mesh->tint[2]);
	glUniform1f(p->u_alpha, mesh->alpha);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, mesh->texture);
	glUniform1i(p->u_tex, 0);

	glVertexAttribPointer((GLuint)p->a_pos, 3, GL_FLOAT, GL_FALSE, stride,
	                      mesh->verts);
	glEnableVertexAttribArray((GLuint)p->a_pos);
	if (p->a_uv >= 0) {
		glVertexAttribPointer((GLuint)p->a_uv, 2, GL_FLOAT, GL_FALSE,
		                      stride, mesh->verts + 3);
		glEnableVertexAttribArray((GLuint)p->a_uv);
	}
	if (p->a_shade >= 0) {
		glVertexAttribPointer((GLuint)p->a_shade, 2, GL_FLOAT, GL_FALSE,
		                      stride, mesh->verts + 5);
		glEnableVertexAttribArray((GLuint)p->a_shade);
	}

	glDrawArrays(GL_TRIANGLES, 0, (GLsizei)(mesh->n_verts
	                                        - mesh->n_verts % 3));

	glDisableVertexAttribArray((GLuint)p->a_pos);
	if (p->a_uv >= 0)
		glDisableVertexAttribArray((GLuint)p->a_uv);
	if (p->a_shade >= 0)
		glDisableVertexAttribArray((GLuint)p->a_shade);
	glBindTexture(GL_TEXTURE_2D, 0);
	glUseProgram(0);
	return TRUE;
}
