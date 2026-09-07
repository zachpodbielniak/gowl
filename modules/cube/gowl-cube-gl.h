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
 * gowl-cube-gl.h -- the shape of the solid and where the camera stands.
 *
 * All the GL plumbing -- context, shaders, textures, passes -- lives in
 * the core effect layer (src/fx/gowl-fx.h), shared with expo, switcher,
 * magnifier and blur.  What is left here is only the part that is about
 * a prism: how wide a side is for a given face count, how far back the
 * camera has to stand for the picture to be steady through a corner, and
 * how a side is lit as it turns away.
 */

#ifndef GOWL_CUBE_GL_H
#define GOWL_CUBE_GL_H

#include <glib.h>

#include "fx/gowl-fx.h"

G_BEGIN_DECLS

struct wlr_buffer;

/* More than a dozen sides and they are too narrow to read on the way
 * past; fewer than three is not a solid. */
#define GOWL_CUBE_GL_MAX_FACES 12

/**
 * GowlCubeFrame:
 * @faces: sides of the prism
 * @dir: +1 or -1, which way the solid turns
 * @rotation: how far it has turned, radians, unsigned
 * @face_angle: radians subtended by one side
 * @bump: 0..1 envelope; 0 must reproduce the flat desktop exactly
 * @speed: 0..1 normalised angular speed, drives motion blur
 * @zoom: configured maximum camera pull-back
 * @pitch_deg: configured maximum camera elevation
 * @shading: 0..1, how dark a side gets as it turns away
 * @reflection: 0..1 floor reflection strength
 * @motion_blur: 0..1 blur strength
 * @caps: draw the top/bottom polygons
 * @backdrop: linear RGB behind the solid
 * @first_slot: lowest slot to consider drawing
 * @last_slot: highest slot to consider drawing
 * @slot: the stored desktop for each slot in [@first_slot, @last_slot];
 *   an entry with a zero texture is drawn as a blank side
 *
 * Everything one frame of the rotation needs.  The renderer makes no
 * decisions of its own: the planner decides what turns and how far, the
 * module decides what a slot contains, and this is the contract.
 */
typedef struct {
	gint     faces;
	gint     dir;
	gdouble  rotation;
	gdouble  face_angle;
	gdouble  bump;
	gdouble  speed;
	gdouble  zoom;
	gdouble  pitch_deg;
	gdouble  shading;
	gdouble  reflection;
	gdouble  motion_blur;
	gboolean caps;
	gfloat   backdrop[3];
	gint     first_slot;
	gint     last_slot;
	const GowlFxTexture *slot;   /* indexed [0 .. last_slot - first_slot] */
} GowlCubeFrame;

/**
 * gowl_cube_draw:
 * @gl: the effect layer's GL context
 * @dst: the buffer to draw into, sized to the output
 * @frame: what to draw
 *
 * Draws one frame of the rotation.
 *
 * Returns: %TRUE on success; on failure @dst is left untouched and the
 *   caller should abandon the rotation rather than show a torn frame.
 */
gboolean gowl_cube_draw (GowlFxGl            *gl,
                         struct wlr_buffer   *dst,
                         const GowlCubeFrame *frame);

G_END_DECLS

#endif /* GOWL_CUBE_GL_H */
