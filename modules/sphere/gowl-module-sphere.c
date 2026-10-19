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
 * Sphere: the desktop is wrapped onto the front of a globe, the globe
 * turns half a revolution, and the next desktop -- mapped onto its far
 * side -- unwraps flat again.
 *
 * Three acts on one clock:
 *
 *   0    .. 0.3   the flat desktop bends onto the front hemisphere as the
 *                 camera eases back to see the whole globe
 *   0.3  .. 0.7   the globe turns pi about its vertical axis
 *   0.7  .. 1     the far hemisphere, now in front, unbends into the
 *                 flat next desktop
 *
 * Each hemisphere is a grid whose every vertex has two places: on the
 * flat desktop and on the sphere.  The morph mixes them, the turn
 * rotates the result about the globe's centre.  The grid for the next
 * desktop starts life as the flat desktop rotated to the back, so the
 * same half turn that brings the globe round brings it face on.
 *
 * Mapping: the equator's half circumference is the desktop's half width,
 * so the middle row is not stretched; latitude covers +-56 degrees, so
 * the top and bottom of a wide desktop are not pinched to the poles.
 *
 * No depth buffer: triangles turned from the camera are dropped (a
 * convex globe then never overlaps itself) and the rest are painted
 * furthest first.  Lighting fades in with the morph, so the flat frames
 * at either end are the desktops exactly.
 */

#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "gowl-sphere"

#include "fx/gowl-fx-transition-module.h"

#include <math.h>
#include <string.h>

#define SPHERE_FOV_Y  (40.0 * G_PI / 180.0)
#define SPHERE_NU     56          /* grid columns per hemisphere */
#define SPHERE_NV     32          /* grid rows */
#define SPHERE_LAT    (56.0 * G_PI / 180.0)
/* Camera pull-back while it is a sphere, as a share of its distance. */
#define SPHERE_PULL   0.2

#define SPHERE_TRIS   (2 * SPHERE_NU * SPHERE_NV * 2)

typedef struct {
	gfloat  z;        /* centroid depth, for the painter's sort */
	gint    side;     /* 0 the desktop left, 1 the one arrived at */
	gfloat  v[3 * GOWL_FX_MESH_STRIDE];
} Tri;

typedef struct {
	Tri    *tris;      /* SPHERE_TRIS */
	Tri   **order;
	gfloat *verts[2];  /* one batch per desktop */
} SphereState;

static gboolean
sphere_setup(GowlFxTransitionRun *run)
{
	SphereState *st = g_new0(SphereState, 1);

	st->tris = g_new0(Tri, SPHERE_TRIS);
	st->order = g_new0(Tri *, SPHERE_TRIS);
	st->verts[0] = g_new0(gfloat, SPHERE_TRIS * 3 * GOWL_FX_MESH_STRIDE);
	st->verts[1] = g_new0(gfloat, SPHERE_TRIS * 3 * GOWL_FX_MESH_STRIDE);
	run->state = st;
	return TRUE;
}

static void
sphere_teardown(GowlFxTransitionRun *run)
{
	SphereState *st = run->state;

	g_free(st->tris);
	g_free(st->order);
	g_free(st->verts[0]);
	g_free(st->verts[1]);
	g_free(st);
	run->state = NULL;
}

static gdouble
smoothstep(gdouble a, gdouble b, gdouble x)
{
	gdouble t = CLAMP((x - a) / (b - a), 0.0, 1.0);

	return t * t * (3.0 - 2.0 * t);
}

/*
 * Grid point (u, v) of hemisphere @side, at morph @m and turn @theta,
 * in world space.  The globe's centre is at (0, 0, -R), so the front of
 * the sphere and the flat desktop both touch z = 0.
 */
static void
grid_point(gdouble u, gdouble v, gint side, gdouble m, gdouble theta,
           gdouble hw, gdouble hh, gdouble R, gdouble *out)
{
	gdouble lon = (u - 0.5) * G_PI;
	gdouble lat = (0.5 - v) * 2.0 * SPHERE_LAT;
	gdouble px, py, pz, sx, sy, sz, x, y, z, c, s;

	/* Flat, and on the sphere, in the globe's own frame. */
	px = (u - 0.5) * 2.0 * hw;
	py = (0.5 - v) * 2.0 * hh;
	pz = 0.0;
	sx = R * cos(lat) * sin(lon);
	sy = R * sin(lat);
	sz = R * cos(lat) * cos(lon) - R;
	if (side == 1) {
		/* The far side: the same grid turned half way round the
		 * centre, so the turn that brings it to the front also brings
		 * it the right way round. */
		px = -px;  pz = -2.0 * R;
		sx = -sx;  sz = -sz - 2.0 * R;
	}
	x = px + (sx - px) * m;
	y = py + (sy - py) * m;
	z = pz + (sz - pz) * m;

	/* The turn, about the vertical axis through the centre. */
	c = cos(theta);
	s = sin(theta);
	out[0] = x * c + (z + R) * s;
	out[1] = y;
	out[2] = -x * s + (z + R) * c - R;
}

static gint
tri_cmp(gconstpointer a, gconstpointer b)
{
	const Tri *x = *(const Tri *const *)a;
	const Tri *y = *(const Tri *const *)b;

	return x->z < y->z ? -1 : (x->z > y->z ? 1 : 0);
}

static void
put_vertex(gfloat *dst, const gdouble *p, gdouble u, gdouble v, gdouble shade)
{
	dst[0] = (gfloat)p[0];
	dst[1] = (gfloat)p[1];
	dst[2] = (gfloat)p[2];
	dst[3] = (gfloat)u;
	dst[4] = (gfloat)v;
	dst[5] = (gfloat)shade;
	dst[6] = 1.0f;
}

static gboolean
sphere_draw(GowlFxPass *pass, GowlFxTransitionRun *run, gdouble eased,
            gdouble linear)
{
	static const gfloat stage[4] = { 0.02f, 0.022f, 0.04f, 1.0f };
	static const gdouble light[3] = { -0.42, 0.52, 0.74 };
	SphereState *st = run->state;
	gdouble w = (gdouble)run->width;
	gdouble h = (gdouble)run->height;
	gdouble hw = w * 0.5, hh = h * 0.5;
	gdouble R = hw * 2.0 / G_PI;
	gdouble m = eased < 0.5 ? smoothstep(0.0, 0.32, eased)
	                        : 1.0 - smoothstep(0.68, 1.0, eased);
	gdouble theta = G_PI * smoothstep(0.26, 0.74, eased) * (gdouble)run->dir;
	gdouble focal = 1.0 / tan(SPHERE_FOV_Y * 0.5);
	gdouble dist = hh * focal * (1.0 + SPHERE_PULL * m);
	gfloat proj[16], view[16], mvp[16];
	gint n = 0, side, i, j, k;
	gint counts[2] = { 0, 0 };
	GowlFxMesh mesh;

	(void)linear;
	for (side = 0; side < 2; side++) {
		for (j = 0; j < SPHERE_NV; j++) {
			for (i = 0; i < SPHERE_NU; i++) {
				gdouble u0 = (gdouble)i / SPHERE_NU;
				gdouble u1 = (gdouble)(i + 1) / SPHERE_NU;
				gdouble v0 = (gdouble)j / SPHERE_NV;
				gdouble v1 = (gdouble)(j + 1) / SPHERE_NV;
				gdouble p00[3], p01[3], p10[3], p11[3];
				const gdouble *q[2][3];
				gdouble quv[2][3][2];
				gint t;

				grid_point(u0, v0, side, m, theta, hw, hh, R, p00);
				grid_point(u0, v1, side, m, theta, hw, hh, R, p01);
				grid_point(u1, v0, side, m, theta, hw, hh, R, p10);
				grid_point(u1, v1, side, m, theta, hw, hh, R, p11);

				q[0][0] = p00; q[0][1] = p01; q[0][2] = p10;
				q[1][0] = p10; q[1][1] = p01; q[1][2] = p11;
				quv[0][0][0] = u0; quv[0][0][1] = v0;
				quv[0][1][0] = u0; quv[0][1][1] = v1;
				quv[0][2][0] = u1; quv[0][2][1] = v0;
				quv[1][0][0] = u1; quv[1][0][1] = v0;
				quv[1][1][0] = u0; quv[1][1][1] = v1;
				quv[1][2][0] = u1; quv[1][2][1] = v1;

				for (t = 0; t < 2; t++) {
					gdouble a[3], b[3], nrm[3], cen[3], to_cam[3];
					gdouble len, lambert, shade;

					for (k = 0; k < 3; k++) {
						a[k] = q[t][1][k] - q[t][0][k];
						b[k] = q[t][2][k] - q[t][0][k];
						cen[k] = (q[t][0][k] + q[t][1][k]
						          + q[t][2][k]) / 3.0;
					}
					/* Outward for both grids: (down) x (along u). */
					nrm[0] = a[1] * b[2] - a[2] * b[1];
					nrm[1] = a[2] * b[0] - a[0] * b[2];
					nrm[2] = a[0] * b[1] - a[1] * b[0];
					to_cam[0] = -cen[0];
					to_cam[1] = -cen[1];
					to_cam[2] = dist - cen[2];
					if (nrm[0] * to_cam[0] + nrm[1] * to_cam[1]
					    + nrm[2] * to_cam[2] <= 0.0)
						continue;
					len = sqrt(nrm[0] * nrm[0] + nrm[1] * nrm[1]
					           + nrm[2] * nrm[2]);
					lambert = len > 0.0
						? (nrm[0] * light[0] + nrm[1] * light[1]
						   + nrm[2] * light[2]) / len
						: 1.0;
					shade = 0.38 + 0.72 * MAX(lambert, 0.0);
					/* Flat at the ends means unlit at the ends. */
					shade = 1.0 + (shade - 1.0) * m;

					st->tris[n].z = (gfloat)cen[2];
					st->tris[n].side = side;
					for (k = 0; k < 3; k++)
						put_vertex(&st->tris[n].v[k * GOWL_FX_MESH_STRIDE],
						           q[t][k], quv[t][k][0],
						           quv[t][k][1], shade);
					st->order[n] = &st->tris[n];
					n++;
				}
			}
		}
	}
	qsort(st->order, (gsize)n, sizeof(Tri *), tri_cmp);

	gowl_fx_mat4_perspective(proj, SPHERE_FOV_Y, w / h,
	                         MAX(1.0, dist * 0.2), dist + R * 4.0);
	gowl_fx_mat4_view(view, dist, 0.0);
	gowl_fx_mat4_multiply(mvp, proj, view);

	gowl_fx_pass_clear(pass, stage);
	gowl_fx_pass_backdrop(pass, stage, (gfloat)m);

	/*
	 * Back to front, one draw per run of triangles from the same side:
	 * the two hemispheres barely interleave (a convex globe), so this
	 * is two or three draws in practice rather than thousands.
	 */
	memset(&mesh, 0, sizeof mesh);
	mesh.mvp = mvp;
	mesh.tint[0] = mesh.tint[1] = mesh.tint[2] = 1.0f;
	mesh.alpha = 1.0f;
	for (i = 0; i < n; i++) {
		Tri *tri = st->order[i];
		gint s = tri->side;

		if (counts[s] > 0 && i > 0 && st->order[i - 1]->side != s) {
			/* The other side's run ended: flush it first. */
			gint o = 1 - s;

			mesh.verts = st->verts[o];
			mesh.n_verts = counts[o] * 3;
			mesh.texture = o == 0 ? run->from.tex : run->to.tex;
			gowl_fx_pass_mesh(pass, &mesh);
			counts[o] = 0;
		}
		memcpy(&st->verts[s][counts[s] * 3 * GOWL_FX_MESH_STRIDE], tri->v,
		       sizeof tri->v);
		counts[s]++;
		if (i + 1 < n && st->order[i + 1]->side != s) {
			mesh.verts = st->verts[s];
			mesh.n_verts = counts[s] * 3;
			mesh.texture = s == 0 ? run->from.tex : run->to.tex;
			gowl_fx_pass_mesh(pass, &mesh);
			counts[s] = 0;
		}
	}
	for (side = 0; side < 2; side++) {
		if (counts[side] == 0)
			continue;
		mesh.verts = st->verts[side];
		mesh.n_verts = counts[side] * 3;
		mesh.texture = side == 0 ? run->from.tex : run->to.tex;
		gowl_fx_pass_mesh(pass, &mesh);
	}
	return TRUE;
}

static const GowlFxTransitionKind sphere_kind = {
	.name           = "sphere",
	.label          = "Sphere",
	/* Three acts: it needs the room. */
	.duration_scale = 2.0,
	.setup          = sphere_setup,
	.teardown       = sphere_teardown,
	.draw           = sphere_draw,
};

GOWL_FX_DEFINE_TRANSITION_MODULE(GowlModuleSphere, gowl_module_sphere,
                                 &sphere_kind, "0.1.0",
                                 "Wraps the desktop onto a globe and turns it to the next tag")
