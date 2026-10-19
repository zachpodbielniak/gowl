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
 * A desktop broken into triangles, for the transitions that shatter,
 * explode or implode it.  Plain geometry on the CPU -- no GL here; the
 * transitions hand the emitted vertices to gowl_fx_pass_mesh().
 *
 * The tessellation is a grid with its inner points jittered and every
 * cell cut along a randomly chosen diagonal.  That is cruder than a
 * Voronoi fracture and looks like broken glass anyway once the pieces
 * spin, and it has the one property that matters: neighbours share
 * their corners EXACTLY, so the shards at rest are the desktop with no
 * hairline cracks at progress 0.
 */

#include "gowl-fx-transition.h"

#include <math.h>
#include <string.h>

GowlFxShard *
gowl_fx_shards_new(gint width, gint height, gint cols, gint rows,
                   guint32 seed, gint *n_out)
{
	GRand *rand;
	gfloat *gx, *gy;
	GowlFxShard *out;
	gint i, j, n = 0;

	g_return_val_if_fail(n_out != NULL, NULL);
	cols = MAX(cols, 1);
	rows = MAX(rows, 1);

	rand = g_rand_new_with_seed(seed);
	gx = g_new(gfloat, (gsize)(cols + 1) * (gsize)(rows + 1));
	gy = g_new(gfloat, (gsize)(cols + 1) * (gsize)(rows + 1));

	/* The grid, inner points jittered up to a third of a cell; edge
	 * points stay on their edge so the outline is the output's. */
	for (j = 0; j <= rows; j++) {
		for (i = 0; i <= cols; i++) {
			gfloat x = (gfloat)width * (gfloat)i / (gfloat)cols;
			gfloat y = (gfloat)height * (gfloat)j / (gfloat)rows;

			if (i > 0 && i < cols)
				x += (gfloat)(g_rand_double_range(rand, -0.33, 0.33)
				              * width / cols);
			if (j > 0 && j < rows)
				y += (gfloat)(g_rand_double_range(rand, -0.33, 0.33)
				              * height / rows);
			gx[j * (cols + 1) + i] = x;
			gy[j * (cols + 1) + i] = y;
		}
	}

	out = g_new0(GowlFxShard, (gsize)cols * (gsize)rows * 2);
	for (j = 0; j < rows; j++) {
		for (i = 0; i < cols; i++) {
			gint a = j * (cols + 1) + i;       /* top-left */
			gint b = a + 1;                    /* top-right */
			gint c = a + (cols + 1);           /* bottom-left */
			gint d = c + 1;                    /* bottom-right */
			gint tri[2][3];
			gint t, k;

			if (g_rand_boolean(rand)) {
				tri[0][0] = a; tri[0][1] = c; tri[0][2] = b;
				tri[1][0] = b; tri[1][1] = c; tri[1][2] = d;
			} else {
				tri[0][0] = a; tri[0][1] = d; tri[0][2] = b;
				tri[1][0] = a; tri[1][1] = c; tri[1][2] = d;
			}
			for (t = 0; t < 2; t++) {
				GowlFxShard *s = &out[n++];

				s->cx = s->cy = 0.0f;
				for (k = 0; k < 3; k++) {
					s->x[k] = gx[tri[t][k]];
					s->y[k] = gy[tri[t][k]];
					s->cx += s->x[k] / 3.0f;
					s->cy += s->y[k] / 3.0f;
				}
				for (k = 0; k < 4; k++)
					s->r[k] = (gfloat)g_rand_double(rand);
			}
		}
	}

	g_free(gx);
	g_free(gy);
	g_rand_free(rand);
	*n_out = n;
	return out;
}

/* Rodrigues: @v turned by @angle about unit @k. */
static void
rotate(const gfloat *k, gfloat angle, gfloat *v)
{
	gfloat c = cosf(angle), s = sinf(angle);
	gfloat d = k[0] * v[0] + k[1] * v[1] + k[2] * v[2];
	gfloat cr[3];
	gint i;

	cr[0] = k[1] * v[2] - k[2] * v[1];
	cr[1] = k[2] * v[0] - k[0] * v[2];
	cr[2] = k[0] * v[1] - k[1] * v[0];
	for (i = 0; i < 3; i++)
		v[i] = v[i] * c + cr[i] * s + k[i] * d * (1.0f - c);
}

void
gowl_fx_shard_emit(gfloat            *dst,
                   const GowlFxShard *shard,
                   gint               width,
                   gint               height,
                   const gfloat      *offset,
                   const gfloat      *axis,
                   gfloat             angle,
                   gfloat             scale,
                   gfloat             shade,
                   gfloat             alpha)
{
	gfloat hw = (gfloat)width * 0.5f, hh = (gfloat)height * 0.5f;
	/* The centroid in the world: centred, +Y up. */
	gfloat wx = shard->cx - hw, wy = hh - shard->cy;
	gint k;

	g_return_if_fail(dst != NULL && shard != NULL);

	for (k = 0; k < 3; k++) {
		gfloat v[3];
		gfloat *o = dst + k * GOWL_FX_MESH_STRIDE;

		v[0] = (shard->x[k] - shard->cx) * scale;
		v[1] = (shard->cy - shard->y[k]) * scale;
		v[2] = 0.0f;
		if (angle != 0.0f && axis != NULL)
			rotate(axis, angle, v);
		o[0] = wx + v[0] + (offset != NULL ? offset[0] : 0.0f);
		o[1] = wy + v[1] + (offset != NULL ? offset[1] : 0.0f);
		o[2] = v[2] + (offset != NULL ? offset[2] : 0.0f);
		o[3] = width > 0 ? shard->x[k] / (gfloat)width : 0.0f;
		o[4] = height > 0 ? shard->y[k] / (gfloat)height : 0.0f;
		o[5] = shade;
		o[6] = alpha;
	}
}

/* ── Drawing ─────────────────────────────────────────────────────── */

#define GOWL_FX_SHARDS_FOV_Y (40.0 * G_PI / 180.0)

typedef struct {
	gfloat z;
	gint   at;     /* index into the vertex scratch, in shards */
} ShardDepth;

static gint
depth_cmp(gconstpointer a, gconstpointer b)
{
	const ShardDepth *x = a, *y = b;

	return x->z < y->z ? -1 : (x->z > y->z ? 1 : 0);
}

gboolean
gowl_fx_transition_draw_shards(GowlFxPass                *pass,
                               const GowlFxTransitionRun *run,
                               const GowlFxShard         *shards,
                               gint                       n,
                               GowlFxShardPlaceFunc       place,
                               gpointer                   user_data,
                               gdouble                    eased,
                               gdouble                    linear,
                               gfloat                     to_scale,
                               gfloat                     to_shade)
{
	static const gfloat stage[4] = { 0.02f, 0.022f, 0.035f, 1.0f };
	gdouble w = (gdouble)run->width, h = (gdouble)run->height;
	gdouble hw = w * 0.5, hh = h * 0.5;
	gdouble dist = hh / tan(GOWL_FX_SHARDS_FOV_Y * 0.5);
	gfloat proj[16], view[16], mvp[16], plane[12];
	gfloat *verts, *sorted;
	ShardDepth *order;
	GowlFxMesh mesh;
	gint i, kept = 0;
	gfloat sx, sy;

	g_return_val_if_fail(pass != NULL && run != NULL, FALSE);
	g_return_val_if_fail(place != NULL, FALSE);

	gowl_fx_mat4_perspective(proj, GOWL_FX_SHARDS_FOV_Y, w / h,
	                         MAX(1.0, dist * 0.04), dist * 6.0);
	gowl_fx_mat4_view(view, dist, 0.0);
	gowl_fx_mat4_multiply(mvp, proj, view);

	/* Behind them, the desktop arrived at, flat at z = 0 -- where it
	 * fills the view exactly at scale 1. */
	gowl_fx_pass_clear(pass, stage);
	sx = (gfloat)(hw * to_scale);
	sy = (gfloat)(hh * to_scale);
	plane[0] = -sx; plane[1]  =  sy; plane[2]  = 0.0f;
	plane[3] = -sx; plane[4]  = -sy; plane[5]  = 0.0f;
	plane[6] =  sx; plane[7]  =  sy; plane[8]  = 0.0f;
	plane[9] =  sx; plane[10] = -sy; plane[11] = 0.0f;
	gowl_fx_transition_pass_desktop(pass, &run->to, mvp, plane, 1.0f,
	                                to_shade);

	verts = g_new(gfloat, (gsize)n * 3 * GOWL_FX_MESH_STRIDE);
	sorted = g_new(gfloat, (gsize)n * 3 * GOWL_FX_MESH_STRIDE);
	order = g_new(ShardDepth, (gsize)n);
	for (i = 0; i < n; i++) {
		gfloat offset[3] = { 0.0f, 0.0f, 0.0f };
		gfloat axis[3] = { 0.0f, 0.0f, 1.0f };
		gfloat angle = 0.0f, scale = 1.0f, shade = 1.0f, alpha = 1.0f;

		if (!place(&shards[i], i, run, eased, linear, offset, axis,
		           &angle, &scale, &shade, &alpha, user_data))
			continue;
		if (alpha <= 0.0f || scale <= 0.0f
		    || offset[2] > (gfloat)(dist * 0.85))
			continue;
		gowl_fx_shard_emit(verts + kept * 3 * GOWL_FX_MESH_STRIDE,
		                   &shards[i], run->width, run->height, offset,
		                   axis, angle, scale, shade, alpha);
		order[kept].z = offset[2];
		order[kept].at = kept;
		kept++;
	}
	qsort(order, (gsize)kept, sizeof(ShardDepth), depth_cmp);
	for (i = 0; i < kept; i++)
		memcpy(sorted + i * 3 * GOWL_FX_MESH_STRIDE,
		       verts + order[i].at * 3 * GOWL_FX_MESH_STRIDE,
		       sizeof(gfloat) * 3 * GOWL_FX_MESH_STRIDE);

	if (kept > 0) {
		memset(&mesh, 0, sizeof mesh);
		mesh.mvp = mvp;
		mesh.verts = sorted;
		mesh.n_verts = kept * 3;
		mesh.texture = run->from.tex;
		mesh.tint[0] = mesh.tint[1] = mesh.tint[2] = 1.0f;
		mesh.alpha = 1.0f;
		gowl_fx_pass_mesh(pass, &mesh);
	}
	g_free(verts);
	g_free(sorted);
	g_free(order);
	return TRUE;
}
