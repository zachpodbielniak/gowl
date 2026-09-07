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
 * gowl-cube-gl.h -- the drawing half of the cube.
 *
 * gowl itself has no renderer and must not gain one (tests/test-no-libregnum.sh
 * enforces that).  This file does not add one either: it borrows the EGL
 * context that wlroots' own GLES2 renderer already owns, draws into a
 * wlr_buffer wlroots allocated, and hands it back.  Nothing here outlives
 * the module, and with any other renderer selected (Vulkan, pixman) the
 * module reports itself unsupported and gets out of the way instead of
 * dragging a second GL stack into the compositor.
 */

#ifndef GOWL_CUBE_GL_H
#define GOWL_CUBE_GL_H

#include <glib.h>

G_BEGIN_DECLS

struct wlr_renderer;
struct wlr_buffer;
struct wlr_texture;

typedef struct _GowlCubeGl GowlCubeGl;

/**
 * GowlCubeFace:
 * @tex: the stored desktop for this slot, or 0 for a blank side
 * @width: pixel width of @tex
 * @height: pixel height of @tex
 *
 * One captured desktop, already owned by the GL layer.
 */
typedef struct {
	guint tex;
	gint  width;
	gint  height;
} GowlCubeFace;

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
 * @slot: the desktop for each slot in [@first_slot, @last_slot]
 *
 * Everything one frame of the rotation needs.  The GL layer makes no
 * decisions of its own: the planner decides what turns and how far, the
 * module decides what a slot contains, and this struct is the contract
 * between them.
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
	const GowlCubeFace *slot;   /* indexed [0 .. last_slot - first_slot] */
} GowlCubeFrame;

/**
 * gowl_cube_gl_supported:
 * @renderer: the compositor's renderer
 *
 * Returns: %TRUE when @renderer is the GLES2 one, which is the only
 *   renderer whose context this can borrow.
 */
gboolean gowl_cube_gl_supported (struct wlr_renderer *renderer);

/**
 * gowl_cube_gl_new:
 * @renderer: the compositor's renderer, borrowed
 *
 * Compiles the shaders and takes the scratch objects.  Returns %NULL when
 * the renderer is not GLES2 or a shader fails to build, which the caller
 * must treat as "no cube", not as an error.
 *
 * Returns: (transfer full) (nullable): a new #GowlCubeGl
 */
GowlCubeGl *gowl_cube_gl_new (struct wlr_renderer *renderer);

/**
 * gowl_cube_gl_free:
 * @self: (transfer full) (nullable): the renderer
 *
 * Must be called while the EGL context is still alive, so before the
 * compositor tears its renderer down --- hence the module's `finish' hook.
 */
void gowl_cube_gl_free (GowlCubeGl *self);

/**
 * gowl_cube_gl_store_face:
 * @self: the renderer
 * @face: (inout): the slot to fill; a non-zero @face->tex is reused
 * @source: a texture of the whole output, as captured from the scene
 * @width: width to store at
 * @height: height to store at
 *
 * Copies a captured desktop into a texture the cube owns.
 *
 * The copy is not busywork.  The captured buffer belongs to the output's
 * swapchain and goes back the moment the capture finishes, and a
 * dma-buf import may arrive as an external-OES texture that the cube's
 * shader cannot sample.  Copying settles both, and because it goes
 * through the same coordinate convention the final draw uses, any
 * disagreement about which end of a buffer is the top cancels out
 * instead of showing up as an upside-down desktop.
 *
 * @width and @height may be smaller than the source: intermediate
 * desktops are only ever seen mid-spin and blurred, so they are stored
 * at half size, which is what keeps an eight-step rotation on a 4K
 * screen from costing a third of a gigabyte.
 *
 * Returns: %TRUE on success.
 */
gboolean gowl_cube_gl_store_face (GowlCubeGl         *self,
                                  GowlCubeFace       *face,
                                  struct wlr_texture *source,
                                  gint                width,
                                  gint                height);

/**
 * gowl_cube_gl_drop_face:
 * @self: the renderer
 * @face: (inout): the slot to release
 *
 * Frees a stored desktop.  Safe on an empty slot.
 */
void gowl_cube_gl_drop_face (GowlCubeGl *self, GowlCubeFace *face);

/**
 * gowl_cube_gl_render:
 * @self: the renderer
 * @dst: the buffer to draw into, sized to the output
 * @frame: what to draw
 *
 * Draws one frame of the rotation into @dst.
 *
 * Returns: %TRUE on success; on failure @dst is left untouched and the
 *   caller should abandon the rotation rather than show a torn frame.
 */
gboolean gowl_cube_gl_render (GowlCubeGl          *self,
                              struct wlr_buffer   *dst,
                              const GowlCubeFrame *frame);

G_END_DECLS

#endif /* GOWL_CUBE_GL_H */
