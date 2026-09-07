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
 * gowl-fx.h -- the shared toolkit the visual-effect modules are built on.
 *
 * gowl adds no renderer of its own and must not (see
 * tests/test-no-libregnum.sh).  What it does have, once wlroots is up, is
 * a GLES2 renderer with an EGL context, and everything here is a way to
 * borrow that safely rather than a second graphics stack:
 *
 *   - a bracket that makes the renderer's context current and puts back
 *     whatever was current before, because that is global state shared
 *     with wlroots and every entry point below respects it;
 *   - textures the effect layer owns, and a copy-in that also settles the
 *     external-image case a dma-buf capture arrives as;
 *   - a pass that draws styled quads into a wlr_buffer;
 *   - a way to capture what an output would look like under a different
 *     set of visible windows, and put the scene back exactly;
 *   - a "sheet": one opaque monitor-sized buffer parked in the scene for
 *     as long as an effect owns that output.
 *
 * FIVE MODULES USE THIS (cube, expo, switcher, magnifier, blur) and none
 * of them contains any of it.  That is the point: the plumbing is where a
 * mistake is expensive and hard to see -- a context left current, a scene
 * node left hidden, a buffer freed after its renderer -- so it lives in
 * one place with one set of tests rather than in five modules with five.
 *
 * NOTHING HERE RUNS WITHOUT A GLES2 RENDERER.  gowl_fx_gl_new() returns
 * NULL under Vulkan or pixman, and every module treats that as "sit this
 * session out", not as an error.
 */

#ifndef GOWL_FX_H
#define GOWL_FX_H

#include <glib.h>
#include <glib-object.h>

#include "gowl-enums.h"

G_BEGIN_DECLS

struct wlr_renderer;
struct wlr_buffer;
struct wlr_texture;
struct wlr_scene_node;
struct wlr_scene_buffer;
struct wlr_scene_tree;
struct wlr_box;

typedef struct _GowlCompositor GowlCompositor;
typedef struct _GowlMonitor GowlMonitor;
typedef struct _GowlClient GowlClient;

/* ── GL context ──────────────────────────────────────────────────── */

typedef struct _GowlFxGl GowlFxGl;

/**
 * gowl_fx_gl_supported:
 * @renderer: the compositor's renderer
 *
 * Returns: %TRUE when @renderer is the GLES2 one, whose context the
 *   effect layer can borrow.  Callers must check this and degrade
 *   gracefully; it is not an error for it to be false.
 */
gboolean gowl_fx_gl_supported (struct wlr_renderer *renderer);

/**
 * gowl_fx_gl_new:
 * @renderer: the compositor's renderer, borrowed
 *
 * Compiles the shared shaders and takes the scratch objects.
 *
 * Returns: (transfer full) (nullable): a context, or %NULL when the
 *   renderer is not GLES2 or a shader failed to build.
 */
GowlFxGl *gowl_fx_gl_new (struct wlr_renderer *renderer);

/**
 * gowl_fx_gl_free:
 * @self: (transfer full) (nullable): the context
 *
 * Must run while the renderer is still alive --- that is, from a scene
 * effect's `finish' hook, not from its finalize.
 */
void gowl_fx_gl_free (GowlFxGl *self);

/* ── Textures the effect layer owns ──────────────────────────────── */

/**
 * GowlFxTexture:
 * @tex: GL texture name, or 0 when empty
 * @width: pixel width
 * @height: pixel height
 */
typedef struct {
	guint tex;
	gint  width;
	gint  height;
} GowlFxTexture;

/**
 * gowl_fx_texture_store:
 * @self: the context
 * @dst: (inout): the texture to fill; reused when already the right size
 * @source: a wlroots texture to copy from
 * @width: width to store at
 * @height: height to store at
 *
 * Copies a wlroots texture into one the effect layer owns.
 *
 * The copy is not busywork.  A captured buffer belongs to the output's
 * swapchain and goes back immediately, and a dma-buf import may arrive as
 * an external-image texture that an ordinary shader cannot sample.
 * Copying settles both.  It also lets @width and @height be smaller than
 * the source, which is how an effect keeps nine full-resolution desktops
 * on a 4K screen from costing a third of a gigabyte.
 *
 * Returns: %TRUE on success.
 */
gboolean gowl_fx_texture_store (GowlFxGl           *self,
                                GowlFxTexture      *dst,
                                struct wlr_texture *source,
                                gint                width,
                                gint                height);

/**
 * gowl_fx_texture_drop:
 * @self: the context
 * @tex: (inout): the texture to release; safe when already empty
 */
void gowl_fx_texture_drop (GowlFxGl *self, GowlFxTexture *tex);

/**
 * gowl_fx_texture_blur:
 * @self: the context
 * @dst: (inout): the texture to write; resized as needed
 * @src: the texture to blur
 * @downscale: how much smaller to work, 1..8
 * @passes: how many box passes, 1..6
 *
 * A downsample-blur-upsample chain, which is how a wide blur is affordable:
 * the cost of a radius is paid by shrinking the image rather than by
 * sampling more of it, and the upscale at the end does the rest.
 *
 * Returns: %TRUE on success.
 */
gboolean gowl_fx_texture_blur (GowlFxGl            *self,
                               GowlFxTexture       *dst,
                               const GowlFxTexture *src,
                               gint                 downscale,
                               gint                 passes);

/* ── Drawing ─────────────────────────────────────────────────────── */

typedef struct _GowlFxPass GowlFxPass;

/**
 * GowlFxQuad:
 * @mvp: 16 floats, column-major; %NULL for a screen-filling quad
 * @pos: four corners as xyz, in strip order (top-left, bottom-left,
 *   top-right, bottom-right); %NULL for a screen-filling quad
 * @uv: four texture coordinates in the same order; %NULL for 0..1
 * @texture: a #GowlFxTexture name, or 0 to fill with @base instead
 * @tint: multiplied into the sampled colour
 * @base: the flat colour used when @texture is 0
 * @blur: texture-space offset per motion-blur tap; {0,0} for none
 * @edge: strength of the lit bevel drawn along the quad's border
 * @edge_width: bevel width in texture coordinates
 * @spec: additive highlight, for a light sweeping across a surface
 * @alpha: overall opacity
 * @fade: above 0.5, fades out towards the quad's top edge (reflections)
 * @corner: rounded-corner radius in texture coordinates; 0 for square
 *
 * One styled quad.  The fields are the union of what the effect modules
 * need from a textured rectangle, so they all share one shader rather
 * than each carrying a near-copy of it.
 */
typedef struct {
	const gfloat *mvp;
	const gfloat *pos;
	const gfloat *uv;
	guint         texture;
	gfloat        tint[3];
	gfloat        base[3];
	gfloat        blur[2];
	gfloat        edge;
	gfloat        edge_width;
	gfloat        spec;
	gfloat        alpha;
	gfloat        fade;
	gfloat        corner;
} GowlFxQuad;

/**
 * gowl_fx_quad_init:
 * @quad: (out): the quad to reset
 *
 * Sets the neutral values --- white tint, full alpha, no bevel, no blur,
 * square corners --- so a caller only assigns what it means to change.
 */
void gowl_fx_quad_init (GowlFxQuad *quad);

/**
 * gowl_fx_pass_begin:
 * @self: the context
 * @dst: the buffer to draw into
 *
 * Makes the renderer's context current, binds @dst and sets up blending.
 * Must be matched by gowl_fx_pass_end().
 *
 * There is no depth buffer, and there will not be one: a wlr_buffer's
 * framebuffer has colour only, and attaching depth to a framebuffer
 * wlroots owns would be reaching into its state.  Effects draw back to
 * front instead.
 *
 * Returns: (transfer full) (nullable): the pass, or %NULL on failure.
 */
GowlFxPass *gowl_fx_pass_begin (GowlFxGl *self, struct wlr_buffer *dst);

/**
 * gowl_fx_pass_clear:
 * @pass: a pass
 * @rgba: four floats
 */
void gowl_fx_pass_clear (GowlFxPass *pass, const gfloat *rgba);

/**
 * gowl_fx_pass_backdrop:
 * @pass: a pass
 * @rgb: the base colour
 * @alpha: overall opacity
 *
 * A pool of light in the middle of the output, fading to near-black at
 * the corners.  Effects that lift the desktop off the screen need
 * something behind it, and a flat fill reads as a bug --- as though the
 * compositor lost the wallpaper --- where a gradient reads as a stage.
 */
void gowl_fx_pass_backdrop (GowlFxPass *pass, const gfloat *rgb, gfloat alpha);

/**
 * gowl_fx_pass_quad:
 * @pass: a pass
 * @quad: what to draw
 */
void gowl_fx_pass_quad (GowlFxPass *pass, const GowlFxQuad *quad);

/**
 * gowl_fx_pass_end:
 * @pass: (transfer full): the pass
 *
 * Flushes, restores the GL state wlroots expects to find, and puts the
 * previous EGL context back.
 *
 * Returns: %TRUE when the pass completed.
 */
gboolean gowl_fx_pass_end (GowlFxPass *pass);

/* ── Scene visibility scratchpad ─────────────────────────────────── */

/**
 * GowlFxVis:
 *
 * A record of scene nodes an effect switched off, so they can be put back
 * exactly.
 *
 * Capturing what an output WOULD look like means lying to the scene for
 * the length of one render --- showing another tag's windows, or one
 * window alone.  The lie has to be undone completely and unconditionally,
 * including on every early return, which is what this exists to make
 * hard to get wrong.
 */
typedef struct _GowlFxVis GowlFxVis;

/**
 * gowl_fx_vis_begin:
 *
 * Returns: (transfer full): an empty scratchpad.
 */
GowlFxVis *gowl_fx_vis_begin (void);

/**
 * gowl_fx_vis_set:
 * @vis: a scratchpad
 * @node: the node to change
 * @enabled: what to set it to
 *
 * Remembers @node's current state the first time it is touched, so
 * repeated changes still restore to the original.
 */
void gowl_fx_vis_set (GowlFxVis *vis, struct wlr_scene_node *node,
                      gboolean enabled);

/**
 * gowl_fx_vis_restore:
 * @vis: (transfer full): the scratchpad
 *
 * Puts every remembered node back and frees the record.
 */
void gowl_fx_vis_restore (GowlFxVis *vis);

/**
 * gowl_fx_vis_show_tags:
 * @vis: a scratchpad
 * @compositor: the compositor
 * @monitor: the output whose clients to filter
 * @tags: the tag mask to show
 *
 * Shows exactly the clients of @monitor that are on @tags.
 *
 * Embedder-pinned clients are skipped, not hidden: they are placed over
 * the embedder's own surface rather than by tag, so guessing here would
 * make them blink.  Use gowl_fx_client_is_pinned() to find them.
 */
void gowl_fx_vis_show_tags (GowlFxVis      *vis,
                            GowlCompositor *compositor,
                            GowlMonitor    *monitor,
                            guint32         tags);

/**
 * gowl_fx_vis_show_only:
 * @vis: a scratchpad
 * @compositor: the compositor
 * @monitor: the output whose clients to filter
 * @client: (nullable): the only client to show, or %NULL for none
 *
 * For capturing one window on its own, which is what a window switcher's
 * previews are.
 */
void gowl_fx_vis_show_only (GowlFxVis      *vis,
                            GowlCompositor *compositor,
                            GowlMonitor    *monitor,
                            GowlClient     *client);

/**
 * gowl_fx_vis_hide_layer:
 * @vis: a scratchpad
 * @compositor: the compositor
 * @layer: which scene layer
 * @keep_pinned: leave embedder-pinned clients in that layer alone
 *
 * Switches a whole scene layer off for a capture.
 *
 * ONLY EVER FOR A CAPTURE.  The layers are shared by every output while
 * tags are per-monitor, so a layer switched off for the length of an
 * effect blanks the other screen.  An effect that needs the layers below
 * it out of the way covers them with a sheet instead.
 */
void gowl_fx_vis_hide_layer (GowlFxVis      *vis,
                             GowlCompositor *compositor,
                             GowlSceneLayer  layer,
                             gboolean        keep_pinned);

/**
 * gowl_fx_client_is_pinned:
 * @client: a client
 *
 * Returns: %TRUE for a client the embedder places over its own surface
 *   rather than by tag --- the in-buffer views of `emacs --gowl', and
 *   module overlays such as a dropdown terminal.
 */
gboolean gowl_fx_client_is_pinned (GowlClient *client);

/* ── Capture ─────────────────────────────────────────────────────── */

/**
 * gowl_fx_capture:
 * @self: the context
 * @compositor: the compositor
 * @monitor: the output to render
 * @out: (inout): the texture to store into
 * @divisor: store at 1/@divisor of the output's size, 1 for full size
 *
 * Renders @monitor's scene AS IT CURRENTLY STANDS into @out.
 *
 * The caller decides what "as it currently stands" means by setting scene
 * visibility with #GowlFxVis first, and must restore it afterwards.  The
 * render is transient: nothing is presented, so a capture may show a
 * state that would be wrong to put on screen.
 *
 * Returns: %TRUE on success.
 */
gboolean gowl_fx_capture (GowlFxGl       *self,
                          GowlCompositor *compositor,
                          GowlMonitor    *monitor,
                          GowlFxTexture  *out,
                          gint            divisor);

/**
 * gowl_fx_capture_to_buffer:
 * @self: the context
 * @compositor: the compositor
 * @monitor: the output to render
 * @out: (out) (transfer full): receives a locked buffer
 *
 * As gowl_fx_capture(), but hands back the wlroots buffer instead of a
 * texture, for an effect that needs to feed the result back into the
 * scene rather than into a shader.
 *
 * Returns: %TRUE on success; unlock @out with wlr_buffer_unlock().
 */
gboolean gowl_fx_capture_to_buffer (GowlFxGl           *self,
                                    GowlCompositor     *compositor,
                                    GowlMonitor        *monitor,
                                    struct wlr_buffer **out);

/* ── Sheet ───────────────────────────────────────────────────────── */

/**
 * GowlFxSheet:
 *
 * One opaque monitor-sized surface an effect draws into, parked in the
 * scene for as long as it owns that output.
 *
 * It hides what is below it by COVERING it, not by switching layers off:
 * the layers are shared between outputs and the tags are not, so on a
 * two-monitor desk switching one off to make room here would blank the
 * other. The few things gowl stacks above the sheet -- fullscreen
 * clients, embedder-pinned overlays -- are taken down individually and
 * restored from a held reference, so a window closed while the effect
 * runs cannot leave a dangling one behind.
 */
typedef struct _GowlFxSheet GowlFxSheet;

/**
 * GowlFxSheetFlags:
 * @GOWL_FX_SHEET_NONE: nothing special
 * @GOWL_FX_SHEET_ABOVE_TOP: park above the bar as well, for an effect
 *   that must cover the whole screen rather than sit under the panel
 * @GOWL_FX_SHEET_KEEP_FULLSCREEN: leave fullscreen clients showing
 */
typedef enum {
	GOWL_FX_SHEET_NONE            = 0,
	GOWL_FX_SHEET_ABOVE_TOP       = 1 << 0,
	GOWL_FX_SHEET_KEEP_FULLSCREEN = 1 << 1
} GowlFxSheetFlags;

/**
 * gowl_fx_sheet_new:
 * @compositor: the compositor
 * @monitor: the output to take
 * @flags: placement options
 *
 * Returns: (transfer full) (nullable): the sheet, or %NULL when the
 *   output cannot be taken (no off-screen buffers, zero-sized output).
 */
GowlFxSheet *gowl_fx_sheet_new (GowlCompositor   *compositor,
                                GowlMonitor      *monitor,
                                GowlFxSheetFlags  flags);

/**
 * gowl_fx_sheet_free:
 * @sheet: (transfer full) (nullable): the sheet
 *
 * Puts the output back --- hidden clients first, then the sheet itself,
 * so no frame can catch the output with neither --- and damages it, since
 * a scene node vanishing is not damage the output would otherwise notice.
 */
void gowl_fx_sheet_free (GowlFxSheet *sheet);

/**
 * gowl_fx_sheet_acquire:
 * @sheet: a sheet
 *
 * Returns: (transfer full) (nullable): a buffer to draw this frame into,
 *   or %NULL if none is free; unlock it after handing it to
 *   gowl_fx_sheet_present().
 */
struct wlr_buffer *gowl_fx_sheet_acquire (GowlFxSheet *sheet);

/**
 * gowl_fx_sheet_present:
 * @sheet: a sheet
 * @buffer: the buffer just drawn
 *
 * Puts @buffer on screen and re-fits the sheet to the output, which may
 * have been reshaped since the last frame.
 */
void gowl_fx_sheet_present (GowlFxSheet *sheet, struct wlr_buffer *buffer);

/**
 * gowl_fx_sheet_set_visible:
 * @sheet: a sheet
 * @visible: whether to show it
 *
 * Hides the sheet without giving up the output.
 *
 * An effect that captures the screen it is drawing ON has to take itself
 * out of the picture first, or it photographs its own last frame and
 * feeds it back --- the visual equivalent of pointing a camera at its own
 * monitor.  The magnifier is exactly that case.
 */
void gowl_fx_sheet_set_visible (GowlFxSheet *sheet, gboolean visible);

/**
 * gowl_fx_texture_set_filter:
 * @self: the context
 * @tex: the texture
 * @smooth: %TRUE for linear sampling, %FALSE for nearest
 *
 * Nearest is not a downgrade here: magnified past 1:1 there is no more
 * detail to be had, and a reader inspecting pixels wants to see the
 * pixels rather than a smeared guess at what is between them.
 */
void gowl_fx_texture_set_filter (GowlFxGl *self, const GowlFxTexture *tex,
                                 gboolean smooth);

/**
 * gowl_fx_sheet_get_monitor:
 * @sheet: a sheet
 *
 * Returns: (transfer none): the output it took.
 */
GowlMonitor *gowl_fx_sheet_get_monitor (GowlFxSheet *sheet);

/**
 * gowl_fx_sheet_get_size:
 * @sheet: a sheet
 * @width: (out) (optional): buffer width in pixels
 * @height: (out) (optional): buffer height in pixels
 *
 * The size of the buffers gowl_fx_sheet_acquire() hands out, which is the
 * output's pixel size and not its logical size.
 */
void gowl_fx_sheet_get_size (GowlFxSheet *sheet, gint *width, gint *height);

/* ── Small matrix helpers ────────────────────────────────────────── */

/**
 * gowl_fx_mat4_identity:
 * @m: (out) (array fixed-size=16): the matrix
 */
void gowl_fx_mat4_identity (gfloat *m);

/**
 * gowl_fx_mat4_multiply:
 * @out: (out) (array fixed-size=16): may alias @a or @b
 * @a: (array fixed-size=16): applied second
 * @b: (array fixed-size=16): applied first
 */
void gowl_fx_mat4_multiply (gfloat *out, const gfloat *a, const gfloat *b);

/**
 * gowl_fx_mat4_perspective:
 * @m: (out) (array fixed-size=16): the matrix
 * @fovy: vertical field of view in radians
 * @aspect: width over height
 * @near_z: near plane
 * @far_z: far plane
 *
 * Perspective WITH Y NEGATED, which is not an accident and must not be
 * "fixed".  wlroots hands out a buffer whose first row is the top of the
 * picture, while GL's first framebuffer row is the bottom; negating Y is
 * what puts world +Y at the top of the result.  It also reverses triangle
 * winding, which is why effects reject back faces themselves rather than
 * with glCullFace --- one place to be right instead of two.
 */
void gowl_fx_mat4_perspective (gfloat *m, gdouble fovy, gdouble aspect,
                               gdouble near_z, gdouble far_z);

/**
 * gowl_fx_mat4_ortho:
 * @m: (out) (array fixed-size=16): the matrix
 * @width: viewport width in the units the caller uses for positions
 * @height: viewport height
 *
 * A flat projection in pixel coordinates with the origin at the top left,
 * for effects that lay things out in screen space.  Carries the same Y
 * negation as gowl_fx_mat4_perspective() and for the same reason.
 */
void gowl_fx_mat4_ortho (gfloat *m, gdouble width, gdouble height);

/**
 * gowl_fx_mat4_view:
 * @m: (out) (array fixed-size=16): the matrix
 * @dist: distance from the origin
 * @pitch: elevation in radians
 *
 * A camera looking at the origin from @dist away and @pitch above the
 * equator.  Check any change against the camera position it implies ---
 * (0, dist*sin(pitch), dist*cos(pitch)) --- rather than by eye: the
 * mirror image of this matrix looks equally plausible in source and puts
 * the camera underneath.
 */
void gowl_fx_mat4_view (gfloat *m, gdouble dist, gdouble pitch);

G_END_DECLS

#endif /* GOWL_FX_H */
