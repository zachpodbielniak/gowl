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
 * THERE IS NO DEPTH BUFFER, AND THAT IS DELIBERATE.  A wlr_buffer's
 * framebuffer has colour only, and attaching depth to a framebuffer
 * wlroots owns would be reaching into its state.  It is not needed: a
 * convex solid seen from outside has no two front-facing sides that
 * overlap on screen, so rejecting back faces on the CPU and drawing
 * backdrop -> reflection -> cap -> sides is already correct.  Anything
 * added later that is NOT part of that convex solid has to justify its
 * own place in that order.
 */

#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "gowl-cube"

#include "gowl-cube-gl.h"

#include <math.h>
#include <string.h>

#include <wlr/types/wlr_buffer.h>

/* Vertical field of view.  A long lens: a wide one throws the corner of
 * the solid far enough forward that the near side visibly swells, which
 * reads as a fisheye rather than as a desktop. */
#define GOWL_CUBE_FOV_Y (42.0 * G_PI / 180.0)

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
            gdouble half_w, gdouble half_h, gboolean mirrored)
{
	gdouble sx = sin(phi), cz = cos(phi);
	gdouble cx = apothem * sx, cz_pos = apothem * cz;
	gdouble ux = cos(phi), uz = -sin(phi);
	gdouble top, bottom;
	gint    i;

	/* Mirrored reflects the side in the plane y = -half_h, so its own
	 * bottom edge ends up against the mirror line. */
	top    = mirrored ? -3.0 * half_h : half_h;
	bottom = mirrored ? -half_h : -half_h;
	if (mirrored)
		bottom = -half_h;

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
		/* Guard against a NaN reaching vertex data and taking the whole
		 * draw call out; a dropped frame beats a hung GPU. */
		if (!isfinite(q->pos[i]))
			q->pos[i] = 0.0f;
	}
}

/* Slot @j's plane angle.  Positive turns the solid so higher tags arrive
 * from the right; the direction flips the whole journey, not the maths. */
static gdouble
slot_phi(const GowlCubeFrame *f, gint j)
{
	return (gdouble)f->dir * ((gdouble)j * f->face_angle - f->rotation);
}

static void
draw_sides(GowlFxPass *pass, const GowlCubeFrame *f, const gfloat *vp,
            gdouble apothem, gdouble half_w, gdouble half_h,
            gdouble cam_y, gdouble cam_z, gboolean reflected)
{
	gdouble light[3];
	gdouble len;
	gint    j;

	/* A fixed key light above and to the left of the viewer.  Fixed, not
	 * attached to the camera: a highlight that travels across the sides
	 * as they turn is most of what says "this is one solid object". */
	light[0] = -0.42; light[1] = 0.62; light[2] = 0.85;
	len = sqrt(light[0] * light[0] + light[1] * light[1]
	           + light[2] * light[2]);
	light[0] /= len; light[1] /= len; light[2] /= len;

	for (j = f->first_slot; j <= f->last_slot; j++) {
		GowlCubeQuad q;
		GowlFxQuad   quad;
		gdouble phi = slot_phi(f, j);
		gdouble nx = sin(phi), nz = cos(phi);
		gdouble vx, vy, vz, vlen, facing;
		gdouble shade, spec, hx, hy, hz, hlen;
		const GowlFxTexture *face = NULL;

		/* View vector from the side's centre to the camera. */
		vx = 0.0 - apothem * nx;
		vy = cam_y;
		vz = cam_z - apothem * nz;
		vlen = sqrt(vx * vx + vy * vy + vz * vz);
		if (vlen <= 0.0)
			continue;
		vx /= vlen; vy /= vlen; vz /= vlen;

		facing = nx * vx + nz * vz;
		/* Back-face rejection, on the CPU (see the note at the top).  The
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

		build_side(&q, phi, apothem, half_w, half_h, reflected);

		gowl_fx_quad_init(&quad);
		quad.mvp = vp;
		quad.pos = q.pos;
		quad.uv  = q.uv;
		quad.texture = face != NULL ? face->tex : 0;
		/* Sides turned away go cool as well as dark.  A purely
		 * multiplicative darkening reads as a dimmer, not as shadow. */
		quad.tint[0] = (gfloat)(shade * (0.90 + 0.10 * shade));
		quad.tint[1] = (gfloat)(shade * (0.94 + 0.06 * shade));
		quad.tint[2] = (gfloat)(shade * (1.10 - 0.10 * shade));
		/* No desktop for this slot -- the journey has run out of tags.
		 * A blank side keeps the solid closed; a hole would show the
		 * backdrop through the middle of the cube. */
		quad.base[0] = f->backdrop[0] * 1.6f + 0.02f;
		quad.base[1] = f->backdrop[1] * 1.6f + 0.02f;
		quad.base[2] = f->backdrop[2] * 1.6f + 0.02f;
		quad.blur[0] = (gfloat)(f->motion_blur * f->speed * 0.010);
		quad.edge = (gfloat)(0.20 * f->bump * facing);
		quad.spec = (gfloat)spec;
		quad.fade = reflected ? 1.0f : 0.0f;
		quad.alpha = reflected
			? (gfloat)(f->reflection * f->bump) : 1.0f;

		gowl_fx_pass_quad(pass, &quad);
	}
}

static void
draw_cap(GowlFxPass *pass, const GowlCubeFrame *f, const gfloat *vp,
          gdouble circumradius, gdouble half_h, gboolean top)
{
	GowlFxQuad quad;
	gint    n = CLAMP(f->faces, 3, GOWL_CUBE_GL_MAX_FACES);
	gint    i;
	gdouble y = top ? half_h : -half_h;
	/* The sides sit at slot angles, so the cap's corners must too, or the
	 * lid is rotated off its own box. */
	gdouble phase = slot_phi(f, 0) + f->face_angle * 0.5;

	gowl_fx_quad_init(&quad);
	quad.mvp = vp;
	quad.base[0] = f->backdrop[0] * 2.4f + 0.03f;
	quad.base[1] = f->backdrop[1] * 2.4f + 0.03f;
	quad.base[2] = f->backdrop[2] * 2.4f + 0.04f;
	quad.edge  = (gfloat)(0.10 * f->bump);
	quad.alpha = (gfloat)f->bump;

	/*
	 * The lid as a fan of triangles, drawn one quad at a time because the
	 * fx pass takes quads.  Each is a degenerate one -- centre, centre,
	 * rim, rim -- which is the same triangle a fan would emit, and it
	 * keeps every effect on the one shared shader.
	 */
	for (i = 0; i < n; i++) {
		gdouble a0 = phase + (gdouble)i * f->face_angle;
		gdouble a1 = a0 + f->face_angle;
		gfloat  pos[12];
		gfloat  uv[8];

		pos[0] = 0.0f; pos[1] = (gfloat)y; pos[2] = 0.0f;
		pos[3] = 0.0f; pos[4] = (gfloat)y; pos[5] = 0.0f;
		pos[6] = (gfloat)(circumradius * sin(a0));
		pos[7] = (gfloat)y;
		pos[8] = (gfloat)(circumradius * cos(a0));
		pos[9]  = (gfloat)(circumradius * sin(a1));
		pos[10] = (gfloat)y;
		pos[11] = (gfloat)(circumradius * cos(a1));

		/*
		 * The shader measures its bevel as the distance to the nearest
		 * edge of the unit square, so pinning x at the middle and running
		 * y from centre to rim turns that expression into a uniform rim
		 * all the way round rather than four bright points.
		 */
		uv[0] = 0.5f; uv[1] = 0.0f;
		uv[2] = 0.5f; uv[3] = 0.0f;
		uv[4] = 0.5f; uv[5] = 1.0f;
		uv[6] = 0.5f; uv[7] = 1.0f;

		quad.pos = pos;
		quad.uv  = uv;
		gowl_fx_pass_quad(pass, &quad);
	}
}

gboolean
gowl_cube_draw(GowlFxGl *gl, struct wlr_buffer *dst, const GowlCubeFrame *frame)
{
	GowlFxPass *pass;
	gint    w, h, faces;
	gdouble half_w, half_h, apothem, circumradius;
	gdouble dist_flat, dist, pitch, near_surface;
	gfloat  proj[16], view[16], vp[16], clear[4];

	if (gl == NULL || dst == NULL || frame == NULL)
		return FALSE;

	w = dst->width;
	h = dst->height;
	if (w <= 0 || h <= 0)
		return FALSE;

	faces  = CLAMP(frame->faces, 3, GOWL_CUBE_GL_MAX_FACES);
	half_w = (gdouble)w * 0.5;
	half_h = (gdouble)h * 0.5;

	/*
	 * Apothem: the distance from the axis to the middle of a side.  It is
	 * fixed by the side's width and the number of sides, which is why a
	 * higher face count makes a fatter, shallower drum.
	 */
	apothem      = half_w / tan(G_PI / (gdouble)faces);
	circumradius = half_w / sin(G_PI / (gdouble)faces);

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
	 * motion nobody asked for.  Measuring from the nearest surface keeps
	 * the apparent size steady through the turn, and since the two agree
	 * exactly when a side is face on, the seamless endpoints survive it.
	 */
	{
		gdouble step = frame->face_angle > 0.0 ? frame->face_angle : G_PI_2;
		gdouble offset = fmod(frame->rotation + step * 0.5, step);
		gdouble to_face;

		if (offset < 0.0)
			offset += step;
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
	 * whichever distance is greater.  Scaled by the envelope so it
	 * contributes nothing at either end, where the picture must still be
	 * exactly the flat desktop.
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

	gowl_fx_mat4_perspective(proj, GOWL_CUBE_FOV_Y,
	                          (gdouble)w / (gdouble)h,
	                          MAX(1.0, (dist - circumradius) * 0.25),
	                          dist + circumradius * 4.0);
	gowl_fx_mat4_view(view, dist, pitch);
	gowl_fx_mat4_multiply(vp, proj, view);

	pass = gowl_fx_pass_begin(gl, dst);
	if (pass == NULL)
		return FALSE;

	/* Opaque clear in the backdrop's own colour: the buffer covers the
	 * whole output, and a transparent one would show the real desktop
	 * underneath the rotation. */
	clear[0] = frame->backdrop[0] * 0.35f;
	clear[1] = frame->backdrop[1] * 0.35f;
	clear[2] = frame->backdrop[2] * 0.35f;
	clear[3] = 1.0f;
	gowl_fx_pass_clear(pass, clear);

	if (frame->bump > 0.0)
		gowl_fx_pass_backdrop(pass, frame->backdrop, (gfloat)frame->bump);

	/*
	 * Painter's order for a convex solid with no depth buffer: whatever
	 * is furthest from the camera first.  The reflection lives below the
	 * floor; the near cap is only drawn when the camera has cleared it,
	 * and is then behind the sides it shares an edge with.
	 */
	if (frame->reflection > 0.0 && frame->bump > 0.0)
		draw_sides(pass, frame, vp, apothem, half_w, half_h,
		           dist * sin(pitch), dist * cos(pitch), TRUE);

	/*
	 * Only when the camera has actually cleared the lid.  Without the
	 * check a shallow pitch still draws the cap, and with no depth buffer
	 * it paints a wedge sticking out above the top edge of the sides ---
	 * the solid appears to grow a hat.
	 */
	if (frame->caps && frame->bump > 0.0
	    && fabs(dist * sin(pitch)) > half_h)
		draw_cap(pass, frame, vp, circumradius, half_h, pitch > 0.0);

	draw_sides(pass, frame, vp, apothem, half_w, half_h,
	           dist * sin(pitch), dist * cos(pitch), FALSE);

	return gowl_fx_pass_end(pass);
}
