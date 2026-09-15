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
 *   - a pass that draws styled quads into a wlr_buffer, one that traces a
 *     ray through a bevelled slab of glass into one, and one that traces
 *     it through a moving water surface;
 *   - a way to capture what an output would look like under a different
 *     set of visible windows, and put the scene back exactly;
 *   - a "sheet": one opaque monitor-sized buffer parked in the scene for
 *     as long as an effect owns that output.
 *
 * SEVEN MODULES USE THIS (cube, expo, switcher, magnifier, blur,
 * liquidglass, liquidwater) and none of them contains any of it.  That is the point: the plumbing is where a
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

/**
 * GowlFxBokehParams:
 * @radius: the defocus disc, in source pixels
 * @downscale: how much smaller to work, 1 to 4.  2 by default; 1 keeps
 *   small highlights intact at four times the cost
 * @samples: taps around the disc, 8 to 128.  Too few and the disc
 *   becomes a ring of dots around a bright spot; too few AND a detailed
 *   wallpaper and the whole thing comes out grainy, because the
 *   highlight weighting leaves only a handful of samples carrying the
 *   weight
 * @blades: aperture blades, 3 to 12; under 3 is a circle (wide open)
 * @rotation: how the aperture is turned, in degrees.  Only visible with
 *   blades, and only worth setting because every highlight in the frame
 *   shares it -- which is one of the cues that says "a lens did this"
 * @highlight: how much more a bright sample counts than a dim one.  A
 *   wallpaper has no values above 1.0, so the dynamic range a real lens
 *   works with has to be put back by hand; 0 is a plain disc average and
 *   looks like a blur with hard edges
 * @threshold: the luminance a sample has to beat to count as bright
 * @edge: spherical aberration -- how much brighter the rim of a disc is
 *   than its middle.  0 is corrected glass, 1 is a soap-bubble bokeh
 *
 * One lens, out of focus.  gowl_fx_bokeh_params_init() fills in a fast
 * six-bladed one.
 */
typedef struct {
	gfloat radius;
	gint   downscale;
	gint   samples;
	gint   blades;
	gfloat rotation;
	gfloat highlight;
	gfloat threshold;
	gfloat edge;
} GowlFxBokehParams;

/**
 * gowl_fx_bokeh_params_init:
 * @params: (out): the parameters to reset
 */
void gowl_fx_bokeh_params_init (GowlFxBokehParams *params);

/**
 * gowl_fx_texture_bokeh:
 * @self: the context
 * @dst: (inout): the texture to write; resized as needed
 * @src: the texture to throw out of focus
 * @params: (nullable): the lens; %NULL is the default one
 *
 * The other kernel the blur module can build its one output-sized
 * picture with.
 *
 * A DISC, not a Gaussian.  An out-of-focus point of light becomes the
 * shape of the APERTURE, evenly filled and hard-edged -- which is the
 * whole visible difference between a photograph's background and a
 * blurred screenshot.  The averaging is done in light rather than in
 * display values, and bright samples are weighted up, because without
 * either of those the discs are present and invisible.
 *
 * Returns: %FALSE when the shader could not be built, which is not an
 *   error --- the caller falls back to the box blur.
 */
gboolean gowl_fx_texture_bokeh (GowlFxGl                *self,
                                GowlFxTexture           *dst,
                                const GowlFxTexture     *src,
                                const GowlFxBokehParams *params);

/**
 * GowlFxCrtParams:
 * @curvature: 1/R of the faceplate, with R in half screen widths.  0 is
 *   a flat panel; 0.45 is a late flat-square consumer tube; past about
 *   0.8 the corners are eating a serious amount of desktop
 * @curvature_y: the vertical share of @curvature, 0 to 1.  1 is a
 *   sphere, 0 is a cylinder --- curved across and dead flat down, which
 *   is a Trinitron
 * @lines: scan lines down the raster, or 0 for
 *   gowl_fx_crt_auto_lines() of the output height
 * @scanline: how deep the dark glass between lines cuts, 0 to 1
 * @beam: the spot's sigma in scan lines with the gun at black
 * @beam_bloom: how much wider the spot gets at full white.  This is the
 *   one that matters: a beam that does not fatten with current draws a
 *   pattern that sits ON the picture rather than being made of it
 * @mask: phosphor triad depth, 0 to 1
 * @mask_kind: which mask the tube has
 * @mask_size: output pixels per triad, or 0 for
 *   gowl_fx_crt_auto_mask_size() of the output width
 * @bloom: halation gain --- light that left the phosphor, bounced
 *   around the faceplate and came back out somewhere else
 * @bloom_cut: the light level halation starts from
 * @vignette: how much darker the edge of the glass is, 0 to 1
 * @corner: the tube's corner radius, as a share of half the screen
 *   height
 * @convergence: how far the red and blue guns miss at the corner, in
 *   output pixels.  Radial, and zero in the middle, where a technician
 *   would have adjusted it
 * @hum: amplitude of the slow bright bar drifting up the screen
 * @gamma: the tube's transfer exponent, near 2.4.  Everything the beam,
 *   the mask and the glow do happens on the light this produces, not on
 *   the drive level that came in
 * @brightness: a final gain, to pay back what the vignette took
 *
 * One cathode ray tube.  gowl_fx_crt_params_init() fills in a consumer
 * set from about 1998.
 */
typedef struct {
	gfloat      curvature;
	gfloat      curvature_y;
	gfloat      lines;
	gfloat      scanline;
	gfloat      beam;
	gfloat      beam_bloom;
	gfloat      mask;
	GowlCrtMask mask_kind;
	gfloat      mask_size;
	gfloat      bloom;
	gfloat      bloom_cut;
	gfloat      vignette;
	gfloat      corner;
	gfloat      convergence;
	gfloat      hum;
	gfloat      gamma;
	gfloat      brightness;
} GowlFxCrtParams;

/**
 * GowlFxCrtClock:
 * @hum_phase: where the mains beat has got to, in [0, 1)
 *
 * The only thing on a tube that moves by itself.  A zeroed clock is a
 * screen that has just been switched on, which is what a renderer loss
 * leaves behind.
 */
typedef struct {
	gdouble hum_phase;
} GowlFxCrtClock;

/**
 * gowl_fx_crt_params_init:
 * @params: (out): the tube to reset
 */
void gowl_fx_crt_params_init (GowlFxCrtParams *params);

/**
 * gowl_fx_crt_advance:
 * @clock: (inout): the clock
 * @dt: seconds since the last frame
 */
void gowl_fx_crt_advance (GowlFxCrtClock *clock, gdouble dt);

/**
 * gowl_fx_crt_auto_lines:
 * @height: the output's height in pixels
 *
 * Returns: the scan line count to use when the config asks for none ---
 *   one line per four output pixels, which is the finest ridge the grid
 *   can carry before the shader has to widen the beam to suit it.
 */
gint gowl_fx_crt_auto_lines (gint height);

/**
 * gowl_fx_crt_auto_mask_size:
 * @width: the output's width in pixels
 *
 * Returns: output pixels per phosphor triad when the config asks for
 *   none.  Scaled with the screen, so the phosphor stays the same size
 *   rather than the same number of pixels.
 */
gint gowl_fx_crt_auto_mask_size (gint width);

/**
 * gowl_fx_crt_fit:
 * @curvature: as #GowlFxCrtParams.curvature
 * @curvature_y: as #GowlFxCrtParams.curvature_y
 * @aspect: the output's height divided by its width
 * @fit_x: (out) (optional): the horizontal stretch at the edge
 * @fit_y: (out) (optional): the vertical stretch at the edge
 *
 * How much the arc stretches the middle of each edge of the picture.
 * The shader divides by these, which is what makes the curved picture
 * FIT the screen instead of overflowing it: the middle of every edge of
 * the raster lands on the middle of the matching edge of the screen and
 * the corners come in from there.  No part of the desktop is pushed off,
 * which a real tube's overscan would have done and which on a desktop
 * eats window buttons.
 *
 * Out here rather than in the shader because it is the same two numbers
 * for every fragment, and because arithmetic in a C file can be tested.
 */
void gowl_fx_crt_fit (gdouble  curvature,
                      gdouble  curvature_y,
                      gdouble  aspect,
                      gfloat  *fit_x,
                      gfloat  *fit_y);

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

/* ── Liquid glass ────────────────────────────────────────────────── */

/**
 * GowlFxGlassParams:
 * @width: the rect's width in pixels
 * @height: the rect's height in pixels
 * @radius: corner radius in pixels
 * @bevel: width of the bent zone along the edge, in pixels; clamped to
 *   half the short side, since past that the direction field the corners
 *   are steered by turns inside out
 * @thickness: how deep the slab is, in pixels.  This is what decides how
 *   far the edge pulls the wallpaper, not @bevel
 * @slope: cap on how fast the displacement decays, px/px.  At 1 the
 *   sampling point stands still; ABOVE it the field folds and the same
 *   wallpaper shows up twice, which is where the liquid look comes from
 * @shape: bevel cross-section --- 0 a quarter circle, 1 a squircle,
 *   2 a raised lip over a shallow dip
 * @dispersion: chromatic aberration, in PIXELS of channel separation.  A
 *   material constant of the glass, unrelated to how strong the lens is
 * @rim: how much light the edge sends back, 0 to 4
 * @shade: how much the edge darkens, 0 to 2
 * @edge_width: how far the shading and the sheen reach, in pixels.
 *   Absolute on purpose: the bright line and the dark hairline under it
 *   are a pixel or two of real glass whatever the bevel is, and scaling
 *   them with it turns a wide rim into a grey band
 * @light: light direction, already as a vector
 * @saturation: saturation inside the bevel ring; the centre is never
 *   touched.  Below 1 cleans up the colour folding and dispersion muddy
 * @clarity: how much the ring shows the UNBLURRED wallpaper, 0 to 1.
 *   Thick glass diffuses and a lens does not, and the ring is the lens
 * @centre_clarity: how much of that clarity the FLAT MIDDLE keeps, 0 to
 *   1.  At 0 the middle is pure frost, which is the other module's job
 *   and is what made glass look like blur
 * @lens: whole-surface magnification, 0 to 0.5.  The slab is very
 *   slightly domed rather than flat; a flat one cannot do anything at
 *   all to its own middle
 * @sheen: strength of the highlight the dome catches.  Zero without
 *   @lens: a flat surface has no slope to catch anything
 * @tint: multiplied into the result
 * @brightness: multiplied into the result
 * @alpha: overall opacity
 * @src_origin: where this rect's top-left sits in the source textures,
 *   in pixels
 * @src_scale: how many SOURCE pixels one pixel of this rect is.  1 when
 *   drawing at full resolution; a rect rendered smaller than the window
 *   it covers is still looking at ALL of that window's wallpaper, and
 *   this is what says so.  0 is read as 1
 *
 * One slab of glass.  gowl_fx_glass_params_init() fills in the tuned
 * defaults; a caller only assigns what it means to change.
 */
typedef struct {
	gint   width, height;
	gfloat radius;
	gfloat bevel;
	gfloat thickness;
	gfloat slope;
	gfloat shape;
	gfloat dispersion;
	gfloat rim;
	gfloat shade;
	gfloat edge_width;
	gfloat light[2];
	gfloat saturation;
	gfloat clarity;
	gfloat centre_clarity;
	gfloat lens;
	gfloat sheen;
	gfloat tint[3];
	gfloat brightness;
	gfloat alpha;
	gfloat src_origin[2];
	gfloat src_scale;
} GowlFxGlassParams;

/**
 * gowl_fx_glass_params_init:
 * @params: (out): the parameters to reset
 */
void gowl_fx_glass_params_init (GowlFxGlassParams *params);

/**
 * gowl_fx_glass_max_displacement:
 * @params: the glass
 *
 * How far the most-displaced pixel of this glass is pulled, in pixels.
 *
 * Exposed because it is the reference the colour fringe and the bevel
 * ring are measured against, and because it is pure arithmetic that can
 * therefore be tested without a GPU.
 *
 * Returns: the maximum displacement, never below a small positive value.
 */
gdouble gowl_fx_glass_max_displacement (const GowlFxGlassParams *params);

/**
 * gowl_fx_pass_glass:
 * @pass: a pass, begun on the buffer the glass is drawn into
 * @soft: the frosted wallpaper, covering the whole output
 * @sharp: (nullable): the same wallpaper unblurred, for the ring; @soft
 *   is used for both when this is %NULL or empty
 * @params: the glass to draw
 *
 * Traces one ray per pixel through a bevelled slab and writes what it
 * finds --- refracted, dispersed, shaded and lit --- over the whole of
 * @pass's buffer, with premultiplied alpha and rounded corners.
 *
 * @soft and @sharp are the WHOLE output's wallpaper, not a crop of it:
 * the rim samples inward from the window's edge, and handing it a crop
 * that starts at the edge leaves it nothing to bend.
 *
 * Returns: %FALSE when the shader could not be built, which is not an
 *   error --- the caller shows no glass and the desktop is as it was.
 */
gboolean gowl_fx_pass_glass (GowlFxPass              *pass,
                             const GowlFxTexture     *soft,
                             const GowlFxTexture     *sharp,
                             const GowlFxGlassParams *params);

/* ── Liquid water ────────────────────────────────────────────────── */

/**
 * GowlFxWaterClock:
 * @phase: the four swell phases, each in [0, 2pi)
 * @drop: the ripple clock, whose integer part is which drop is falling
 *
 * Where the waves have got to.
 *
 * It is four phases and not a time, which is not fussiness.  A
 * seconds-since-start float loses its mantissa: after an hour a 32-bit
 * float resolves about a quarter of a second, and the waves visibly
 * stutter.  Wrapping the time instead makes every wave jump at once,
 * because the four run at incommensurate rates and no wrap point is a
 * whole number of cycles for all of them.  Accumulating each phase in a
 * double and wrapping it into the range a float represents exactly is the
 * only one of the three that never goes wrong.
 *
 * Advance it with gowl_fx_water_advance(); a zeroed clock is still water.
 */
typedef struct {
	gdouble phase[4];
	gdouble drop;
} GowlFxWaterClock;

/**
 * gowl_fx_water_advance:
 * @clock: (inout): the clock
 * @dt: seconds since the last advance
 * @speed: how fast the water runs; 1.0 is the tuned rate
 *
 * Moves the waves on.  A @dt over a quarter of a second is treated as a
 * quarter of a second: coming back from a stall --- a tag switch, a VT
 * switch, a laptop lid --- should not teleport the sea.
 */
void gowl_fx_water_advance (GowlFxWaterClock *clock,
                            gdouble           dt,
                            gdouble           speed);

/**
 * GowlFxWaterParams:
 * @width: the rect's width in pixels
 * @height: the rect's height in pixels
 * @radius: corner radius in pixels
 * @amplitude: wave height in pixels.  Everything that reads as "how rough
 *   is it" is ultimately this and @choppiness
 * @wavelength: the longest wave's length in pixels; the other three are
 *   derived from it
 * @choppiness: 0 is a pure sine --- a swell; towards 1 the troughs flatten
 *   and the crests narrow, which is what wind does to one
 * @depth: how far the refracted ray travels before it reaches the
 *   wallpaper, in pixels.  This is what decides how much the water bends
 *   what is behind it
 * @drops: how many expanding rings, 0 to 6.  A calm pool IS its ripples;
 *   a sea has none
 * @drop_amp: how tall those rings are, relative to @amplitude
 * @shore: how far from the window's edge the water calms, in pixels.  0
 *   runs the waves straight into the edge
 * @dispersion: chromatic aberration in pixels of channel separation
 * @specular: strength of the glint on the crests.  A surface that
 *   refracts but never catches the light reads as warped glass, not as a
 *   liquid
 * @shine: specular exponent; higher is a tighter, harder glint
 * @fresnel: how much the surface reflects at grazing angles
 * @reflect: how far along the normal the faked reflection reaches, in
 *   pixels
 * @caustics: strength of the light gathered where the surface is concave
 * @foam: whitecaps on the crests, 0 to 1
 * @meniscus: light along the waterline where the surface climbs the edge
 * @light: direction to the light, as a 3-vector
 * @tint: what the water takes out of the light
 * @absorption: how much of @tint is applied, 0 to 1
 * @clarity: how much of the UNFROSTED wallpaper shows through, 0 to 1
 * @brightness: multiplied into the result
 * @alpha: overall opacity
 * @src_origin: where this rect's top-left sits in the source textures, in
 *   pixels
 * @src_scale: how many SOURCE pixels one pixel of this rect is.  1 when
 *   drawing at full resolution; a rect rendered smaller than the window
 *   it covers is still looking at ALL of that window's wallpaper, and
 *   this is what says so.  0 is read as 1
 *
 * One body of water.  gowl_fx_water_params_init() fills in a quiet pond.
 */
typedef struct {
	gint   width, height;
	gfloat radius;
	gfloat amplitude;
	gfloat wavelength;
	gfloat choppiness;
	gfloat depth;
	gfloat drops;
	gfloat drop_amp;
	gfloat shore;
	gfloat dispersion;
	gfloat specular;
	gfloat shine;
	gfloat fresnel;
	gfloat reflect;
	gfloat caustics;
	gfloat foam;
	gfloat meniscus;
	gfloat light[3];
	gfloat tint[3];
	gfloat absorption;
	gfloat clarity;
	gfloat brightness;
	gfloat alpha;
	gfloat src_origin[2];
	gfloat src_scale;
} GowlFxWaterParams;

/**
 * gowl_fx_water_params_init:
 * @params: (out): the parameters to reset
 */
void gowl_fx_water_params_init (GowlFxWaterParams *params);

/**
 * gowl_fx_pass_water:
 * @pass: a pass, begun on the buffer the water is drawn into
 * @soft: the frosted wallpaper, covering the whole output
 * @sharp: (nullable): the same wallpaper unblurred
 * @params: the water to draw
 * @clock: (nullable): where the waves have got to; %NULL is still water
 *
 * Builds a height field, takes its normal and its curvature from five
 * samples, refracts one ray per pixel through it at n = 1.333, and writes
 * what it finds --- bent, tinted, lit and foamed --- over the whole of
 * @pass's buffer, with premultiplied alpha and rounded corners.
 *
 * Unlike every other pass here this one is NEVER up to date: the caller
 * is expected to draw it again next frame.
 *
 * Returns: %FALSE when the shader could not be built, which is not an
 *   error --- the caller shows no water and the desktop is as it was.
 */
gboolean gowl_fx_pass_water (GowlFxPass              *pass,
                             const GowlFxTexture     *soft,
                             const GowlFxTexture     *sharp,
                             const GowlFxWaterParams *params,
                             const GowlFxWaterClock  *clock);

/* ── Liquid rain ─────────────────────────────────────────────────── */

/**
 * GOWL_FX_RAIN_CYCLES:
 *
 * How many runs, and how many drop lives, before the rain repeats.
 *
 * The clocks are wrapped at this and the shader takes its run and life
 * indexes mod it, which is what makes both continuous across the wrap.
 * Larger repeats later and quantises the head position more coarsely,
 * because a float has to hold the clock; at 256 a given column sees a
 * run come round again after tens of minutes, and the head steps to
 * within a fifth of a pixel.
 */
#define GOWL_FX_RAIN_CYCLES (256.0)

/**
 * GowlFxRainClock:
 * @life: the resting drops' lifecycle clock, in [0, %GOWL_FX_RAIN_CYCLES)
 * @run: the three running-drop clocks, each in the same range
 *
 * Where the rain has got to.
 *
 * CYCLES rather than a time, and rather than the water's radians.  The
 * fractional part is where a drop has got to; the WHOLE part is which
 * drop it is -- which run is going down this column, which life this
 * cell is on -- and the shader hashes against that, so a track is not
 * used by the same drop forever.  Without it the rain is a screensaver:
 * the same handful of columns running the same drop down the same line,
 * and a third of the window that never has one at all.
 *
 * Wrapping at a whole number of cycles is what keeps both halves
 * continuous there: the fraction is unchanged across the wrap, and the
 * whole part changes by a multiple of %GOWL_FX_RAIN_CYCLES, which is
 * zero once the shader has taken it mod the same number.
 *
 * The one rule this brings with it: any multiplier applied to one of
 * these INSIDE the shader must be a whole number, or that layer snaps at
 * every wrap.  The per-column speed there is 1, 2 or 3 for exactly this
 * reason.
 *
 * Advance it with gowl_fx_rain_advance(); a zeroed clock is a pane that
 * has only just started to wet.
 */
typedef struct {
	gdouble life;
	gdouble run[3];
	/*
	 * The storm, which is the rain with the lightning switched on.
	 *
	 * SECONDS here, not cycles, and that is not an oversight.  The rest
	 * of this clock is cyclic because it drives a field that repeats;
	 * a flash is a one-off event with a hard onset and a decay measured
	 * in tens of milliseconds, so what the shader needs is "how bright
	 * is it RIGHT NOW", worked out on the CPU where a double has
	 * precision to spare.
	 *
	 * @flash is that number, 0 to 1.  @strike_t is how far into the
	 * current flash we are and is negative while none is happening;
	 * @wait is how long until the next one; @strike is which flash,
	 * which is what the stroke pattern is hashed from; @bolt is where
	 * across the sky it is, -1 to 1, so the glints move.
	 */
	gdouble flash;
	gdouble strike_t;
	gdouble wait;
	gdouble strike;
	gdouble bolt;
} GowlFxRainClock;

/**
 * gowl_fx_rain_lightning_advance:
 * @clock: (inout): the clock, whose @flash and @bolt this writes
 * @dt: seconds since the last advance
 * @rate: mean seconds between strikes; 0 or less switches the lightning
 *   off and decays any flash in progress to nothing
 * @power: how bright a flash gets, 1.0 being the tuned strength
 *
 * Moves the storm on.
 *
 * A FLASH IS NOT ONE FLASH.  What people picture as a lightning flash is
 * three to five separate return strokes down the same channel, tens of
 * milliseconds apart, and the flicker between them is the single most
 * recognisable thing about it -- a smooth fade in and out reads as
 * somebody turning a lamp up, which is what almost every implementation
 * of this does.  Each stroke here rises in a millisecond and decays with
 * its own time constant, and the count, the spacing and the strengths
 * are hashed from @clock->strike so no two flashes are the same one
 * twice.
 *
 * The gaps are exponential, which is what a Poisson process gives and
 * what a storm sounds like: sometimes two almost together, sometimes a
 * long wait.  A fixed interval is the other thing that gives it away.
 */
void gowl_fx_rain_lightning_advance (GowlFxRainClock *clock,
                                     gdouble          dt,
                                     gdouble          rate,
                                     gdouble          power);

/**
 * gowl_fx_rain_advance:
 * @clock: (inout): the clock
 * @dt: seconds since the last advance
 * @speed: how fast the running drops fall; 1.0 is the tuned rate
 * @life_seconds: how long a resting drop lives, from condensing to
 *   sliding away
 *
 * Moves the rain on.  A @dt over a quarter of a second is treated as a
 * quarter of a second: coming back from a stall --- a tag switch, a VT
 * switch, a laptop lid --- should not teleport every drop down the pane.
 */
void gowl_fx_rain_advance (GowlFxRainClock *clock,
                           gdouble          dt,
                           gdouble          speed,
                           gdouble          life_seconds);

/**
 * GowlFxRainParams:
 * @width: the rect's width in pixels
 * @height: the rect's height in pixels
 * @radius: corner radius in pixels
 * @cell: pixels per cell of the fine resting-drop layer.  Everything
 *   about a drop is a fraction of this, so it is the one number that
 *   means "how big is the rain"
 * @density: how many cells hold a drop at all, 0 to 1.  Well under half
 *   on purpose: a jittered grid reads as random only while it is sparse
 * @bulge: how domed a drop is.  1 is a hemisphere; lower is a bead that
 *   has spread, higher is a marble
 * @depth: how far the refracted ray travels before it reaches the
 *   wallpaper, in multiples of the DROP'S OWN RADIUS.  Radii and not
 *   pixels: a lens shows an inverted image of what is a few of its own
 *   widths behind it, so a figure in pixels makes a small drop sample
 *   something tens of widths away and come back one flat colour.  Around
 *   2.5 inverts; below 1 merely shifts
 * @dispersion: chromatic separation inside a drop
 * @runs: how many columns have a drop running down them, 0 to 1
 * @run_width: pixels per column of the fine running layer
 * @run_len: pixels of trail behind a running head
 * @beads: how much of a trail is left behind as residual drops, 0 to 1
 * @fog: how frosted the DRY pane is, 0 to 1.  The drops lift this, which
 *   is the whole reason the effect does not look like the blur with
 *   spots painted on it
 * @clarity: how much of the fog a drop lifts, 0 to 1
 * @specular: strength of the glint on each drop
 * @shine: specular exponent; higher is a tighter, harder glint
 * @rim: how much darker the edge of a drop is than its middle
 * @impact: strength of the ring a landing drop throws
 * @light: direction to the light, as a 3-vector
 * @tint: what the water takes out of the light
 * @absorption: how much of @tint is applied, 0 to 1.  Small: rain is not
 *   a swimming pool
 * @brightness: multiplied into the result
 * @alpha: overall opacity
 * @src_origin: where this rect's top-left sits in the source textures,
 *   in pixels
 * @src_scale: how many SOURCE pixels one pixel of this rect is.  1 when
 *   drawing at full resolution; a rect rendered smaller than the window
 *   it covers is still looking at ALL of that window's wallpaper, and
 *   this is what says so.  0 is read as 1
 * @seed: which crop of the rain this rect shows.  Any number; it is
 *   wrapped.  The pattern is anchored to the RECT, because the drops are
 *   on that window rather than on the screen behind it -- so without a
 *   seed every window shows the same drops in the same places, which two
 *   windows side by side make obvious at once
 *
 * One rainy pane.  gowl_fx_rain_params_init() fills in a steady shower.
 */
typedef struct {
	gint   width, height;
	gfloat radius;
	gfloat cell;
	gfloat density;
	gfloat bulge;
	gfloat depth;
	gfloat dispersion;
	gfloat runs;
	gfloat run_width;
	gfloat run_len;
	gfloat beads;
	gfloat fog;
	gfloat clarity;
	gfloat specular;
	gfloat shine;
	gfloat rim;
	gfloat impact;
	/* How lit the pane is by lightning this instant, 0 to 1, and where
	 * across the sky the bolt is, -1 to 1.  Both come from the clock's
	 * @flash and @bolt; 0 is a pane with no storm over it, which is what
	 * gowl_fx_rain_params_init() leaves. */
	gfloat flash;
	gfloat bolt;
	gfloat light[3];
	gfloat tint[3];
	gfloat absorption;
	gfloat brightness;
	gfloat alpha;
	gfloat src_origin[2];
	gfloat src_scale;
	gfloat seed;
} GowlFxRainParams;

/**
 * gowl_fx_rain_params_init:
 * @params: (out): the parameters to reset
 */
void gowl_fx_rain_params_init (GowlFxRainParams *params);

/**
 * gowl_fx_pass_rain:
 * @pass: a pass, begun on the buffer the rain is drawn into
 * @soft: the frosted wallpaper, covering the whole output
 * @sharp: (nullable): the same wallpaper unblurred
 * @params: the rain to draw
 * @clock: (nullable): where the rain has got to; %NULL is a pane that
 *   has only just started to wet
 *
 * Draws a field of water drops on a frosted pane, each one a spherical
 * cap refracting the wallpaper at n = 1.333, over the whole of @pass's
 * buffer, with premultiplied alpha and rounded corners.
 *
 * Resting drops grow, sit and slide away; running drops fall down
 * wandering columns leaving beaded trails that clear the frost behind
 * them.  All of it is a function of the clock and the pixel --- there is
 * no simulation state anywhere, which is what lets any frame be drawn
 * without the one before it.
 *
 * Unlike every other pass here except the water, this one is NEVER up to
 * date: the caller is expected to draw it again next frame.
 *
 * Returns: %FALSE when the shader could not be built, which is not an
 *   error --- the caller shows no rain and the desktop is as it was.
 */
gboolean gowl_fx_pass_rain (GowlFxPass             *pass,
                            const GowlFxTexture    *soft,
                            const GowlFxTexture    *sharp,
                            const GowlFxRainParams *params,
                            const GowlFxRainClock  *clock);

/* ── Dew on a web ────────────────────────────────────────────────── */

/**
 * GOWL_FX_DEW_CYCLES:
 *
 * How far the breathing and shimmer clocks run before repeating.
 *
 * Nothing is indexed by the whole part here --- a web does not emit
 * anything --- so this is only a bound on how large the numbers the
 * sines see are allowed to get.
 */
#define GOWL_FX_DEW_CYCLES (64.0)

/**
 * GowlFxDewClock:
 * @sway: three breathing phases, each in [0, %GOWL_FX_DEW_CYCLES)
 * @shimmer: the per-drop glint phase, in the same range
 *
 * Where the web has got to.
 *
 * Advance it with gowl_fx_dew_advance(); a zeroed clock is a web in
 * still air.
 */
typedef struct {
	gdouble sway[3];
	gdouble shimmer;
} GowlFxDewClock;

/**
 * gowl_fx_dew_advance:
 * @clock: (inout): the clock
 * @dt: seconds since the last advance
 * @speed: how much air is moving; 1.0 is the tuned rate
 *
 * Moves the web on.  A @dt over a quarter of a second is treated as a
 * quarter of a second.
 */
void gowl_fx_dew_advance (GowlFxDewClock *clock,
                          gdouble         dt,
                          gdouble         speed);

/**
 * GowlFxDewParams:
 * @width: the rect's width in pixels
 * @height: the rect's height in pixels
 * @radius: corner radius in pixels
 * @radials: how many spokes the web has, 3 to 48.  Their angles are
 *   jittered: a web whose spokes are exactly 360/n apart is a wheel
 * @pitch: pixels the capture spiral gains per turn
 * @thread: silk width in pixels
 * @drop: a bead's radius in pixels.  Capped against @spacing and
 *   @pitch, because a bead that reaches its neighbour or the next turn
 *   welds the web into a disc
 * @spacing: pixels between beads along the thread.  This is the
 *   Rayleigh-Plateau wavelength: a coated fibre does not stay coated,
 *   it breaks into a regular string of drops
 * @sag: how far a loaded span hangs, in pixels
 * @depth: how far the wallpaper is behind, in a drop's own radii.  Past
 *   2 a drop inverts what is behind it, which is what a ball lens does
 * @bulge: how domed a drop reads
 * @dispersion: chromatic separation through a drop
 * @silk: how bright the dry thread is.  BRIGHT, not dark: silk is a
 *   transparent fibre and what reaches the eye from it is scattered
 *   light
 * @glint: the highlight on each drop
 * @shine: specular exponent
 * @rim: how much darker the very edge of a drop is
 * @sway: how far the web breathes, in pixels
 * @hub: where the hub sits, 0 to 1 of the pane.  Above centre by
 *   default, because that is where an orb weaver builds it
 * @fog: how misted the air is
 * @clarity: how much of that a drop lifts
 * @light: direction to the light, as a 3-vector
 * @tint: what the water takes out of the light
 * @absorption: how much of @tint is applied
 * @brightness: multiplied into the result
 * @alpha: overall opacity
 * @src_origin: where this rect's top-left sits in the source textures
 * @src_scale: how many SOURCE pixels one pixel of this rect is
 * @seed: which web this rect shows
 *
 * One orb web, wet.  gowl_fx_dew_params_init() fills in a web at dawn.
 */
typedef struct {
	gint   width, height;
	gfloat radius;
	gfloat radials;
	gfloat pitch;
	gfloat thread;
	gfloat drop;
	gfloat spacing;
	gfloat sag;
	gfloat depth;
	gfloat bulge;
	gfloat dispersion;
	gfloat silk;
	gfloat glint;
	gfloat shine;
	gfloat rim;
	gfloat sway;
	gfloat hub[2];
	gfloat fog;
	gfloat clarity;
	gfloat light[3];
	gfloat tint[3];
	gfloat absorption;
	gfloat brightness;
	gfloat alpha;
	gfloat src_origin[2];
	gfloat src_scale;
	gfloat seed;
} GowlFxDewParams;

/**
 * gowl_fx_dew_params_init:
 * @params: (out): the parameters to reset
 */
void gowl_fx_dew_params_init (GowlFxDewParams *params);

/**
 * gowl_fx_pass_dew:
 * @pass: a pass, begun on the buffer the web is drawn into
 * @soft: the clouded wallpaper, covering the whole output
 * @sharp: (nullable): the same wallpaper unblurred
 * @params: the web to draw
 * @clock: (nullable): where it has got to
 *
 * Draws an orb web across the window, strung with dew: radials from a
 * hub above centre, an Archimedean capture spiral sagging between them,
 * and evenly spaced beads on the spiral --- each of them a lens.
 *
 * The optics are the rain's.  The placement is the effect.
 *
 * Returns: %FALSE when the shader could not be built, which is not an
 *   error --- the caller shows no web and the desktop is as it was.
 */
gboolean gowl_fx_pass_dew (GowlFxPass            *pass,
                           const GowlFxTexture   *soft,
                           const GowlFxTexture   *sharp,
                           const GowlFxDewParams *params,
                           const GowlFxDewClock  *clock);

/* ── Under water ─────────────────────────────────────────────────── */

/**
 * GOWL_FX_SUBMERGED_CYCLES:
 *
 * How far the caustic and drift clocks run before repeating.
 *
 * Nothing here is indexed by a large multiple of the cycle -- the motes
 * are the only thing with a whole part, and it picks one speck -- so
 * this can be generous without costing precision.
 */
#define GOWL_FX_SUBMERGED_CYCLES (128.0)

/**
 * GowlFxSubmergedClock:
 * @caustic: three caustic phases, each in [0, %GOWL_FX_SUBMERGED_CYCLES)
 * @drift: the marine snow's fall clock, in the same range
 * @surf: the surface's own phase, for the shafts and the band at the top
 *
 * Where the water has got to.
 *
 * Advance it with gowl_fx_submerged_advance(); a zeroed clock is water
 * that has only just been looked at.
 */
typedef struct {
	gdouble caustic[3];
	gdouble drift;
	gdouble surf;
} GowlFxSubmergedClock;

/**
 * gowl_fx_submerged_advance:
 * @clock: (inout): the clock
 * @dt: seconds since the last advance
 * @speed: how fast the surface overhead moves; 1.0 is the tuned rate
 * @drift_seconds: how long a speck takes to cross the pane
 *
 * Moves the water on.  A @dt over a quarter of a second is treated as a
 * quarter of a second.
 */
void gowl_fx_submerged_advance (GowlFxSubmergedClock *clock,
                                gdouble               dt,
                                gdouble               speed,
                                gdouble               drift_seconds);

/**
 * GowlFxSubmergedParams:
 * @width: the rect's width in pixels
 * @height: the rect's height in pixels
 * @radius: corner radius in pixels
 * @depth: METRES of water between the eye and the wallpaper, at the
 *   middle of the pane.  The one knob that matters: everything about the
 *   colour follows from it through @extinction
 * @extinction: per-metre absorption in red, green and blue.  These three
 *   numbers ARE the colour of the effect --- there is no blue tint
 *   anywhere in it.  Their ratio is the point: red dies about twenty
 *   times faster than blue in clear water
 * @murk: extra scattering, as though the water were silty.  Raises the
 *   extinction and the haze together
 * @water: the colour scattered back INTO the line of sight, which is
 *   what stops distant things being black
 * @caustics: how bright the net of surface-focused light is
 * @caustic_scale: how many caustic cells fit across the pane
 * @shafts: columns of light coming down from the surface
 * @shaft_lean: how far they lean, which is where the sun is
 * @motes: how much marine snow, 0 to 1
 * @mote_size: a speck's radius in pixels
 * @surface: how visible the underside of the surface is at the top of
 *   the pane, 0 to 1
 * @sway: how far the whole view wobbles, in pixels
 * @fog: how hazy the water is before @murk is added
 * @clarity: how much of that is lifted
 * @brightness: multiplied into the result
 * @alpha: overall opacity
 * @src_origin: where this rect's top-left sits in the source textures
 * @src_scale: how many SOURCE pixels one pixel of this rect is
 * @seed: which crop of the field this rect shows
 *
 * One view from under water.  gowl_fx_submerged_params_init() fills in
 * a few metres of clear coastal water.
 */
typedef struct {
	gint   width, height;
	gfloat radius;
	gfloat depth;
	gfloat extinction[3];
	gfloat murk;
	gfloat water[3];
	gfloat caustics;
	gfloat caustic_scale;
	gfloat shafts;
	gfloat shaft_lean;
	gfloat motes;
	gfloat mote_size;
	gfloat surface;
	gfloat sway;
	gfloat fog;
	gfloat clarity;
	gfloat brightness;
	gfloat alpha;
	gfloat src_origin[2];
	gfloat src_scale;
	gfloat seed;
} GowlFxSubmergedParams;

/**
 * gowl_fx_submerged_params_init:
 * @params: (out): the parameters to reset
 */
void gowl_fx_submerged_params_init (GowlFxSubmergedParams *params);

/**
 * gowl_fx_pass_submerged:
 * @pass: a pass, begun on the buffer the water is drawn into
 * @soft: the clouded wallpaper, covering the whole output
 * @sharp: (nullable): the same wallpaper unblurred
 * @params: the water to draw
 * @clock: (nullable): where it has got to
 *
 * Draws the wallpaper as though the eye were under water: wavelength
 * absorption with depth, light scattered back in, a moving caustic net
 * from the surface above, shafts, and marine snow drifting down.
 *
 * The liquid water next door is a SURFACE seen from outside.  This is
 * the medium seen from inside, and none of what a medium does over a
 * distance is anything a surface does.
 *
 * Like every animated backdrop here, this is NEVER up to date.
 *
 * Returns: %FALSE when the shader could not be built, which is not an
 *   error --- the caller shows no water and the desktop is as it was.
 */
gboolean gowl_fx_pass_submerged (GowlFxPass                  *pass,
                                 const GowlFxTexture         *soft,
                                 const GowlFxTexture         *sharp,
                                 const GowlFxSubmergedParams *params,
                                 const GowlFxSubmergedClock  *clock);

/* ── Embers ──────────────────────────────────────────────────────── */

/**
 * GOWL_FX_EMBERS_CYCLES:
 *
 * How many sparks a column lets go before the clock repeats.
 *
 * The same size as the fizz's and for the same reason: an ember is
 * indexed by EMISSION NUMBER, which is the clock times the column's
 * emissions per cycle, so the integer the hash sees is several times
 * this rather than this.
 */
#define GOWL_FX_EMBERS_CYCLES (64.0)

/**
 * GowlFxEmbersClock:
 * @rise: the three ember clocks, each in [0, %GOWL_FX_EMBERS_CYCLES)
 * @haze: three heat-shimmer phases, in the same range
 *
 * Where the fire has got to.
 *
 * Cycles, as everywhere here: the fraction of a rise clock is how far up
 * a spark has got and the whole part is WHICH spark, so a column does
 * not let go of the same one forever.
 *
 * Advance it with gowl_fx_embers_advance(); a zeroed clock is a fire
 * that has just been lit.
 */
typedef struct {
	gdouble rise[3];
	gdouble haze[3];
} GowlFxEmbersClock;

/**
 * gowl_fx_embers_advance:
 * @clock: (inout): the clock
 * @dt: seconds since the last advance
 * @speed: how fast the sparks rise; 1.0 is the tuned rate
 * @haze_speed: how fast the heat shimmer drifts
 *
 * Moves the fire on.  A @dt over a quarter of a second is treated as a
 * quarter of a second: a stall is not a draught.
 */
void gowl_fx_embers_advance (GowlFxEmbersClock *clock,
                             gdouble            dt,
                             gdouble            speed,
                             gdouble            haze_speed);

/**
 * GowlFxEmbersParams:
 * @width: the rect's width in pixels
 * @height: the rect's height in pixels
 * @radius: corner radius in pixels
 * @column: pixels per column of the ember layer
 * @density: how many columns carry sparks at all, 0 to 1
 * @ember: a spark's radius in pixels.  Capped against @column
 * @spacing: how closely a column lets them go, 0 to 1.  Quantised inside
 *   the shader to a whole number per cycle
 * @sway: how far a spark wanders sideways, in pixels.  Capped at a
 *   quarter of @column, because the layer only asks three columns
 * @drag: how hard a spark decelerates as it rises.  Larger is a spark
 *   that leaps and then crawls; 0 would be constant speed, which reads
 *   as rain going the wrong way
 * @temperature: KELVIN at birth.  This is the palette: the colour of
 *   every spark is the Planckian locus at its current temperature, so
 *   2300 is a wood fire and 1400 is a dying one
 * @cool: how fast a spark cools over its rise.  Its brightness follows
 *   the FOURTH POWER of the temperature, so this is a far stronger knob
 *   than it looks
 * @flicker: per-spark unsteadiness, 0 to 2
 * @ash: the fraction that have burnt out --- dark, and FALLING
 * @glow: the halo around each spark
 * @hearth: the fire's own light on the bottom of the pane
 * @haze: heat shimmer, in pixels of displacement at the bottom
 * @haze_scale: how many shimmer cells fit across the pane
 * @fog: how smoky the pane is
 * @clarity: how much of that clears towards the top
 * @tint: a warm cast over the whole pane
 * @brightness: multiplied into the result
 * @alpha: overall opacity
 * @src_origin: where this rect's top-left sits in the source textures
 * @src_scale: how many SOURCE pixels one pixel of this rect is
 * @seed: which crop of the field this rect shows
 *
 * One fire, below the window.  gowl_fx_embers_params_init() fills in a
 * hearth.
 */
typedef struct {
	gint   width, height;
	gfloat radius;
	gfloat column;
	gfloat density;
	gfloat ember;
	gfloat spacing;
	gfloat sway;
	gfloat drag;
	gfloat temperature;
	gfloat cool;
	gfloat flicker;
	gfloat ash;
	gfloat glow;
	gfloat hearth;
	gfloat haze;
	gfloat haze_scale;
	gfloat fog;
	gfloat clarity;
	gfloat tint[3];
	gfloat brightness;
	gfloat alpha;
	gfloat src_origin[2];
	gfloat src_scale;
	gfloat seed;
} GowlFxEmbersParams;

/**
 * gowl_fx_embers_params_init:
 * @params: (out): the parameters to reset
 */
void gowl_fx_embers_params_init (GowlFxEmbersParams *params);

/**
 * gowl_fx_pass_embers:
 * @pass: a pass, begun on the buffer the fire is drawn into
 * @soft: the clouded wallpaper, covering the whole output
 * @sharp: (nullable): the same wallpaper unblurred
 * @params: the fire to draw
 * @clock: (nullable): where it has got to; %NULL is a fire just lit
 *
 * Draws the wallpaper through the heat of a fire below the window:
 * sparks rising and slowing, cooling along the Planckian locus and
 * dimming as the fourth power of their temperature, ash falling back
 * through them, and the whole view shimmering in the hot air.
 *
 * Nothing here is a lens.  The sparks ADD light, and the shimmer is a
 * domain warp rather than a refraction --- which is what hot air
 * actually does to a view through it.
 *
 * Like every animated backdrop here, this is NEVER up to date.
 *
 * Returns: %FALSE when the shader could not be built, which is not an
 *   error --- the caller shows no fire and the desktop is as it was.
 */
gboolean gowl_fx_pass_embers (GowlFxPass               *pass,
                              const GowlFxTexture      *soft,
                              const GowlFxTexture      *sharp,
                              const GowlFxEmbersParams *params,
                              const GowlFxEmbersClock  *clock);

/* ── The soap film ───────────────────────────────────────────────── */

/**
 * GOWL_FX_SOAP_CYCLES:
 *
 * How many films are blown, drained and popped before the clock repeats.
 *
 * Small, because unlike the rain and the fizz nothing is indexed by
 * anything larger than the cycle number itself: the whole part picks
 * where the hole opens and nothing else, so thirty-two is a long time
 * before the same film comes round and a number a float has no trouble
 * with.
 */
#define GOWL_FX_SOAP_CYCLES (32.0)

/**
 * GowlFxSoapClock:
 * @life: which film, and how far through its life, in
 *   [0, %GOWL_FX_SOAP_CYCLES)
 * @swirl: three plume phases, each in the same range
 *
 * Where the film has got to.
 *
 * The fractional part of @life is a film's whole existence -- blown,
 * draining, black at the top, popped -- and the whole part is which
 * film it is, which is what decides where the hole opens.  The three
 * @swirl phases advect the thin patches upward at rates with no common
 * period.
 *
 * Advance it with gowl_fx_soap_advance(); a zeroed clock is a film that
 * has just been blown.
 */
typedef struct {
	gdouble life;
	gdouble swirl[3];
} GowlFxSoapClock;

/**
 * gowl_fx_soap_advance:
 * @clock: (inout): the clock
 * @dt: seconds since the last advance
 * @life_seconds: how long one film lasts, blown to popped
 * @swirl_speed: how fast the thin patches rise; 1.0 is the tuned rate
 *
 * Moves the film on.  A @dt over a quarter of a second is treated as a
 * quarter of a second: a stall is not a reason for the film to pop.
 */
void gowl_fx_soap_advance (GowlFxSoapClock *clock,
                           gdouble          dt,
                           gdouble          life_seconds,
                           gdouble          swirl_speed);

/**
 * GowlFxSoapParams:
 * @width: the rect's width in pixels
 * @height: the rect's height in pixels
 * @radius: corner radius in pixels
 * @thickness: NANOMETRES at the bottom of a freshly blown film.  This is
 *   the setting: the colour of every pixel is a function of the local
 *   thickness and nothing else, so this decides how many interference
 *   orders are stacked up the pane.  900 is about five
 * @thin: the top's thickness as a fraction of the bottom's.  Gravity
 *   drains a film downward, and this is how steep the wedge is
 * @drain: how fast the whole film thins over its life.  Larger means the
 *   black cap creeps down sooner
 * @turbulence: how much the rising thin patches disturb the bands, 0 to 2
 * @swirl: how many plume cells fit across the pane
 * @index: refractive index of the liquid.  1.35 is soapy water; 1.47 is
 *   oil on water, which is the same physics and a different look
 * @gain: how much the real reflectance is amplified.  A soap film sends
 *   back at most 9% of what hits it, which is correct and nearly
 *   invisible behind a desktop window; this is the honest amplification
 *   of a real number rather than a painted rainbow
 * @sheen: how bright the room reflected in the film is
 * @wedge: how far the wallpaper is displaced by the film acting as a
 *   prism, in pixels per unit of thickness slope
 * @dispersion: chromatic separation through that prism
 * @pop: how much of a film's life is spent popping, 0 to 0.6.  0 is a
 *   film that never pops, which is not a thing soap does
 * @meniscus: the thick border where the film meets its frame, 0 to 2
 * @fog: how turbid the liquid is
 * @clarity: how much of that the film lifts
 * @light: direction to the light, as a 3-vector
 * @tint: what the liquid takes out of the light
 * @brightness: multiplied into the result
 * @alpha: overall opacity
 * @src_origin: where this rect's top-left sits in the source textures
 * @src_scale: how many SOURCE pixels one pixel of this rect is
 * @seed: which crop of the field this rect shows
 *
 * One soap film.  gowl_fx_soap_params_init() fills in a film a few
 * seconds old.
 */
typedef struct {
	gint   width, height;
	gfloat radius;
	gfloat thickness;
	gfloat thin;
	gfloat drain;
	gfloat turbulence;
	gfloat swirl;
	gfloat index;
	gfloat gain;
	gfloat sheen;
	gfloat wedge;
	gfloat dispersion;
	gfloat pop;
	gfloat meniscus;
	gfloat fog;
	gfloat clarity;
	gfloat light[3];
	gfloat tint[3];
	gfloat brightness;
	gfloat alpha;
	gfloat src_origin[2];
	gfloat src_scale;
	gfloat seed;
} GowlFxSoapParams;

/**
 * gowl_fx_soap_params_init:
 * @params: (out): the parameters to reset
 */
void gowl_fx_soap_params_init (GowlFxSoapParams *params);

/**
 * gowl_fx_pass_soap:
 * @pass: a pass, begun on the buffer the film is drawn into
 * @soft: the clouded wallpaper, covering the whole output
 * @sharp: (nullable): the same wallpaper unblurred
 * @params: the film to draw
 * @clock: (nullable): where it has got to; %NULL is a film just blown
 *
 * Draws the wallpaper through a soap film: horizontal interference bands
 * that drain downward, thin patches rising through them, a black cap
 * spreading from the top, and a pop.
 *
 * The colour is not painted.  It is 4 R0 sin^2(2 pi n d / lambda) at
 * three wavelengths, for the local thickness d --- which is why the top
 * of an old film goes black rather than fading out, and why what you see
 * THROUGH the film is the exact complement of what bounces off it.
 *
 * Like every animated backdrop here, this is NEVER up to date: the
 * caller is expected to draw it again next frame.
 *
 * Returns: %FALSE when the shader could not be built, which is not an
 *   error --- the caller shows no film and the desktop is as it was.
 */
gboolean gowl_fx_pass_soap (GowlFxPass             *pass,
                            const GowlFxTexture    *soft,
                            const GowlFxTexture    *sharp,
                            const GowlFxSoapParams *params,
                            const GowlFxSoapClock  *clock);

/* ── Cathode ray tube ──────────────────────────────── */

/**
 * gowl_fx_pass_crt:
 * @pass: the pass, whose target is the whole output
 * @screen: the finished desktop, captured this frame
 * @glow: (nullable): the same picture blurred wide, for halation.  %NULL
 *   turns the glow off rather than failing
 * @params: the tube
 * @clock: (nullable): where the mains beat is; %NULL is a still screen
 *
 * The desktop, on a tube.  The ONLY whole-output filter in here:
 * everything else in this header draws what is behind a window, and
 * this draws what is in front of all of them.
 *
 * Returns: %FALSE when the shader could not be built, which leaves the
 *   caller to present the capture unfiltered or put its sheet away.
 */
gboolean gowl_fx_pass_crt (GowlFxPass            *pass,
                           const GowlFxTexture   *screen,
                           const GowlFxTexture   *glow,
                           const GowlFxCrtParams *params,
                           const GowlFxCrtClock  *clock);

/* ── Carbonation ─────────────────────────────────────────────────── */

/**
 * GOWL_FX_FIZZ_CYCLES:
 *
 * How many bubble trains, and how many clinging lives, before the fizz
 * repeats.
 *
 * Smaller than the rain's 256 and for a reason the rain does not have: a
 * train is indexed by EMISSION NUMBER, which is the clock times the
 * column's rate times its bubbles-per-cycle -- so the integer the shader
 * hashes against is up to @GOWL_FX_FIZZ_CYCLES * 3 * 8 rather than the
 * clock itself.  At 64 that tops out around 1500, where a float still
 * separates neighbouring emissions by twenty thousand times the
 * arithmetic's own step.  At 256 it would not.
 */
#define GOWL_FX_FIZZ_CYCLES (64.0)

/**
 * GowlFxFizzClock:
 * @cling: the clinging bubbles' lifecycle clock, in
 *   [0, %GOWL_FX_FIZZ_CYCLES)
 * @rise: the three rising-bubble clocks, each in the same range
 *
 * Where the fizz has got to.
 *
 * Cycles rather than seconds, exactly as #GowlFxRainClock: the
 * fractional part is how far up a bubble has got and the whole part is
 * WHICH bubble it is, so a nucleation site does not emit the same bubble
 * forever.  The same one rule comes with it -- any multiplier applied to
 * one of these inside the shader must be a whole number, or that layer
 * snaps at every wrap -- and the same two places obey it: the per-column
 * rise rate is 1, 2 or 3, and the bubbles-per-cycle of a train is an
 * integer from 1 to 8.
 *
 * Advance it with gowl_fx_fizz_advance(); a zeroed clock is a drink
 * that has only just been poured.
 */
typedef struct {
	gdouble cling;
	gdouble rise[3];
} GowlFxFizzClock;

/**
 * gowl_fx_fizz_advance:
 * @clock: (inout): the clock
 * @dt: seconds since the last advance
 * @speed: how fast the bubbles rise; 1.0 is the tuned rate
 * @cling_seconds: how long a bubble clings to the glass before it
 *   detaches
 *
 * Moves the fizz on.  A @dt over a quarter of a second is treated as a
 * quarter of a second, for the reason gowl_fx_rain_advance() gives: a
 * stall is not a reason for every bubble to teleport to the top.
 */
void gowl_fx_fizz_advance (GowlFxFizzClock *clock,
                           gdouble          dt,
                           gdouble          speed,
                           gdouble          cling_seconds);

/**
 * GowlFxFizzParams:
 * @width: the rect's width in pixels
 * @height: the rect's height in pixels
 * @radius: corner radius in pixels
 * @cell: pixels per cell of the clinging-bubble layer, and the ruler
 *   every bubble radius is a fraction of
 * @bubble: bubble radius as a fraction of @cell at the moment of
 *   release, before it has grown
 * @growth: how much bigger a bubble is at the top of the pane than at
 *   the bottom.  0 is a bubble that does not grow, 1 doubles it.  This
 *   is also what makes a train SPREAD as it rises, because the rise rate
 *   goes as the square of the radius
 * @sites: how many columns hold a nucleation site, 0 to 1
 * @site_width: pixels per column of the fine train layer
 * @spacing: how closely a site emits, 0 to 1.  Quantised inside the
 *   shader to a whole number of bubbles per cycle
 * @stray: how many columns carry a loose bubble that did not come from a
 *   site, 0 to 1
 * @cling: how many cells hold a bubble stuck to the glass, 0 to 1
 * @wobble: how far a bubble wanders sideways as it rises, in pixels.
 *   Applied only above the size at which a real bubble's path goes
 *   unstable, so the small ones go straight up and the big ones zigzag
 * @foam: the head at the top of the pane, 0 to 1
 * @foam_depth: how far down the pane the head reaches, in pixels
 * @depth: how far the wallpaper is behind the pane, in multiples of a
 *   bubble's OWN RADIUS.  A gas bubble in liquid is a DIVERGING lens, so
 *   unlike the rain's drops this never inverts however large it is --- it
 *   minifies, which is what a bubble in a glass actually does
 * @dispersion: chromatic separation through a bubble
 * @mirror: strength of the silvered ring around a bubble.  Light inside
 *   the liquid meeting the bubble past the critical angle is totally
 *   reflected, and for water against air that is the outer QUARTER of the
 *   disc.  It is the most recognisable thing about a bubble and the first
 *   thing a naive implementation leaves out
 * @fog: how cloudy the drink is, 0 to 1
 * @clarity: how much of that a bubble lifts, 0 to 1
 * @specular: strength of the glint on each bubble
 * @shine: specular exponent; higher is a tighter, harder glint
 * @rim: how much darker the very edge of a bubble is
 * @light: direction to the light, as a 3-vector
 * @tint: what the drink takes out of the light
 * @absorption: how much of @tint is applied, 0 to 1
 * @brightness: multiplied into the result
 * @alpha: overall opacity
 * @src_origin: where this rect's top-left sits in the source textures,
 *   in pixels
 * @src_scale: how many SOURCE pixels one pixel of this rect is; 0 is
 *   read as 1
 * @seed: which crop of the fizz this rect shows.  Any number; it is
 *   wrapped
 *
 * One glass of something carbonated.  gowl_fx_fizz_params_init() fills
 * in a soda.
 */
typedef struct {
	gint   width, height;
	gfloat radius;
	gfloat cell;
	gfloat bubble;
	gfloat growth;
	gfloat sites;
	gfloat site_width;
	gfloat spacing;
	gfloat stray;
	gfloat cling;
	gfloat wobble;
	gfloat foam;
	gfloat foam_depth;
	gfloat depth;
	gfloat dispersion;
	gfloat mirror;
	gfloat fog;
	gfloat clarity;
	gfloat specular;
	gfloat shine;
	gfloat rim;
	gfloat light[3];
	gfloat tint[3];
	gfloat absorption;
	gfloat brightness;
	gfloat alpha;
	gfloat src_origin[2];
	gfloat src_scale;
	gfloat seed;
} GowlFxFizzParams;

/**
 * gowl_fx_fizz_params_init:
 * @params: (out): the parameters to reset
 */
void gowl_fx_fizz_params_init (GowlFxFizzParams *params);

/**
 * gowl_fx_pass_fizz:
 * @pass: a pass, begun on the buffer the fizz is drawn into
 * @soft: the clouded wallpaper, covering the whole output
 * @sharp: (nullable): the same wallpaper unblurred
 * @params: the fizz to draw
 * @clock: (nullable): where the fizz has got to; %NULL is a drink just
 *   poured
 *
 * Draws the wallpaper through a glass of something carbonated: trains of
 * bubbles streaming from nucleation sites on the glass, loose bubbles
 * drifting up between them, bubbles clinging to the pane and growing
 * until they let go, and a head of foam at the top.
 *
 * Each bubble is a gas sphere in liquid, which is a diverging lens with a
 * totally-internally-reflecting outer quarter --- it minifies rather than
 * inverting, and it is ringed in silver.  All of it is a function of the
 * clock and the pixel; there is no simulation state anywhere.
 *
 * Like the water and the rain, this is NEVER up to date: the caller is
 * expected to draw it again next frame.
 *
 * Returns: %FALSE when the shader could not be built, which is not an
 *   error --- the caller shows no fizz and the desktop is as it was.
 */
gboolean gowl_fx_pass_fizz (GowlFxPass             *pass,
                            const GowlFxTexture    *soft,
                            const GowlFxTexture    *sharp,
                            const GowlFxFizzParams *params,
                            const GowlFxFizzClock  *clock);

/* ── Falling leaves ──────────────────────────────────────────────── */

/**
 * GOWL_FX_LEAF_CYCLES:
 *
 * How many falls, and how many tenures on the glass, before the leaves
 * repeat.
 *
 * The same trade as the rain's: larger repeats later and quantises a
 * leaf's position more coarsely.  128 puts a given column's next leaf
 * some minutes away and still steps a leaf to well inside a pixel.
 */
#define GOWL_FX_LEAF_CYCLES (128.0)

/**
 * GowlFxLeafClock:
 * @stick: the stuck leaves' tenure clock, in [0, %GOWL_FX_LEAF_CYCLES)
 * @fall: the three falling layers' clocks, each in the same range
 * @gust: how hard the wind is blowing right now, 0 to 1
 * @sway: the shiver phase, in radians, that everything already on the
 *   glass moves to
 * @breeze: the three phases the gust is built from, in radians
 *
 * Where the wind has got to.
 *
 * @gust is the reason this struct is not just three more cycle clocks.
 * The wind is a CONTINUOUS quantity that every leaf on every screen
 * reads at once, and that is exactly what makes a gust look like ONE
 * gust rather than like every leaf independently deciding to leave.  It
 * is derived, not integrated: @breeze and @sway are what advance, and
 * @gust is read off them.
 *
 * Those are kept in RADIANS and wrapped at 2*pi individually, which is
 * the only wrapping that is exact for them --- a single phase scaled by
 * three different factors would jump at every wrap, because the scaled
 * values do not land on a whole turn together.  Wrapping each on its own
 * costs two doubles and is correct forever.
 *
 * Advance it with gowl_fx_leaf_advance(); a zeroed clock is a still
 * morning with a clean window.
 */
typedef struct {
	gdouble stick;
	gdouble fall[3];
	gdouble gust;
	gdouble sway;
	gdouble breeze[3];
} GowlFxLeafClock;

/**
 * gowl_fx_leaf_advance:
 * @clock: (inout): the clock
 * @dt: seconds since the last advance
 * @speed: how fast the leaves fall; 1.0 is the tuned rate
 * @tenure_seconds: how long a leaf stays on the glass in still air
 * @gustiness: how hard and how often the wind gets up, 0 to 2
 *
 * Moves the leaves on, and blows the wind.
 *
 * The gust is one slow sine modulated by two slower ones and then
 * squared, so it is calm most of the time and strong briefly --- wind
 * that spends half its life at half strength is not wind, it is a fan.
 * A @dt over a quarter of a second is treated as a quarter of a second.
 */
void gowl_fx_leaf_advance (GowlFxLeafClock *clock,
                           gdouble          dt,
                           gdouble          speed,
                           gdouble          tenure_seconds,
                           gdouble          gustiness);

/**
 * GowlFxLeafParams:
 * @width: the rect's width in pixels
 * @height: the rect's height in pixels
 * @radius: corner radius in pixels
 * @leaf: a leaf's radius in pixels, from the middle of the blade to the
 *   tip.  The one number that means "how big are the leaves"
 * @cell: pixels per cell of the stuck-leaf layer
 * @stuck: how many cells hold a leaf resting on the glass, 0 to 1
 * @column: pixels per column of the fine falling layer
 * @falling: how many columns have a leaf coming down them, 0 to 1
 * @flutter: how far a falling leaf swings sideways, as a fraction of a
 *   column
 * @tumble: how fast a falling leaf turns over.  A leaf edge-on is a
 *   line, and that periodic collapse to nothing is the single clearest
 *   sign that a thing on screen is a leaf and not a sticker
 * @wind: the steady sideways drift, in pixels per fall
 * @gust_push: how far a gust throws things, in pixels
 * @curl: how much a leaf has dried and curled, 0 to 1.  A curled leaf
 *   touches the glass only in the middle, which is what its shadow says
 * @veins: strength of the venation, 0 to 1
 * @translucency: how much of the wallpaper comes through a leaf, 0 to 1.
 *   A leaf on a window is BACKLIT, which is why it glows rather than
 *   sitting there as a brown shape
 * @gloss: how wet the leaves are; scales the specular
 * @shadow: how dark the contact shadow under a stuck leaf is, 0 to 1
 * @fog: how hazy the pane itself is, 0 to 1
 * @clarity: how much of that haze a leaf's wet contact patch lifts
 * @shine: specular exponent
 * @tint_warm: the colour of a freshly-turned leaf (reds)
 * @tint_gold: the colour of a leaf at its peak (oranges and yellows)
 * @tint_dry: the colour of a leaf that has been down a while (browns)
 * @light: direction to the light, as a 3-vector
 * @brightness: multiplied into the result
 * @alpha: overall opacity
 * @src_origin: where this rect's top-left sits in the source textures
 * @src_scale: how many SOURCE pixels one pixel of this rect is
 * @seed: which crop of the fall this rect shows
 *
 * One window in autumn.  gowl_fx_leaf_params_init() fills in a steady
 * fall on a breezy day.
 */
typedef struct {
	gint   width, height;
	gfloat radius;
	gfloat leaf;
	gfloat cell;
	gfloat stuck;
	gfloat column;
	gfloat falling;
	gfloat flutter;
	gfloat tumble;
	gfloat wind;
	gfloat gust_push;
	gfloat curl;
	gfloat veins;
	gfloat translucency;
	gfloat gloss;
	gfloat shadow;
	gfloat fog;
	gfloat clarity;
	gfloat shine;
	gfloat tint_warm[3];
	gfloat tint_gold[3];
	gfloat tint_dry[3];
	gfloat light[3];
	gfloat brightness;
	gfloat alpha;
	gfloat src_origin[2];
	gfloat src_scale;
	gfloat seed;
} GowlFxLeafParams;

/**
 * gowl_fx_leaf_params_init:
 * @params: (out): the parameters to reset
 */
void gowl_fx_leaf_params_init (GowlFxLeafParams *params);

/**
 * gowl_fx_pass_leaves:
 * @pass: a pass, begun on the buffer the leaves are drawn into
 * @soft: the hazed wallpaper, covering the whole output
 * @sharp: (nullable): the same wallpaper unblurred
 * @params: the fall to draw
 * @clock: (nullable): where the wind has got to; %NULL is a still
 *   morning
 *
 * Draws autumn leaves falling past a window and collecting on it.
 *
 * A falling leaf flutters, tumbles edge-on and back, and drifts with the
 * wind.  A leaf that has landed lies flat against the glass with a
 * contact shadow under it and shivers when the wind gets up; when a gust
 * is strong enough it peels from one edge, pivots about its stem and
 * goes.  Leaves are backlit, so the wallpaper comes through them tinted
 * and the venation shows dark.
 *
 * Like the water and the rain, this is NEVER up to date.
 *
 * Returns: %FALSE when the shader could not be built, which is not an
 *   error --- the caller shows no leaves and the desktop is as it was.
 */
gboolean gowl_fx_pass_leaves (GowlFxPass             *pass,
                              const GowlFxTexture    *soft,
                              const GowlFxTexture    *sharp,
                              const GowlFxLeafParams *params,
                              const GowlFxLeafClock  *clock);

/* ── Snow ────────────────────────────────────────────────────────── */

/**
 * GOWL_FX_SNOW_CYCLES:
 *
 * How many falls, how many settled lives and how many melt-water runs
 * before the snow repeats.  The rain's trade, and the rain's number.
 */
#define GOWL_FX_SNOW_CYCLES (256.0)

/**
 * GowlFxSnowClock:
 * @settle: the settled flakes' lifecycle clock --- land, sit, melt, run
 *   --- in [0, %GOWL_FX_SNOW_CYCLES)
 * @fall: the three falling layers' clocks, each in the same range
 * @run: the two melt-water running clocks, each in the same range
 * @frost: how far the frost has grown, 0 upwards; unwrapped and used
 *   only as a smooth parameter
 *
 * Where the snow has got to.
 *
 * Cycles for the same reason as the rain: the fraction is where a flake
 * has got to and the whole part is which flake it is.  @frost is not a
 * cycle clock --- frost grows and does not repeat --- so it is a plain
 * parameter that the shader only ever uses smoothly, and the advance
 * holds it inside a range a float can still resolve.
 *
 * Advance it with gowl_fx_snow_advance(); a zeroed clock is a warm pane
 * in still air.
 */
typedef struct {
	gdouble settle;
	gdouble fall[3];
	gdouble run[2];
	gdouble frost;
} GowlFxSnowClock;

/**
 * gowl_fx_snow_advance:
 * @clock: (inout): the clock
 * @dt: seconds since the last advance
 * @speed: how fast the flakes fall; 1.0 is the tuned rate
 * @life_seconds: how long a settled flake takes to land, sit, melt and
 *   run away
 * @frost_rate: how fast frost creeps in from the edges; 0 never frosts
 *
 * Moves the snow on.  A @dt over a quarter of a second is treated as a
 * quarter of a second.
 */
void gowl_fx_snow_advance (GowlFxSnowClock *clock,
                           gdouble          dt,
                           gdouble          speed,
                           gdouble          life_seconds,
                           gdouble          frost_rate);

/**
 * GowlFxSnowParams:
 * @width: the rect's width in pixels
 * @height: the rect's height in pixels
 * @radius: corner radius in pixels
 * @flake: a falling flake's radius in pixels
 * @cell: pixels per cell of the settled layer
 * @settled: how many cells hold a flake resting on the glass, 0 to 1
 * @column: pixels per column of the fine falling layer
 * @falling: how many columns have a flake coming down them, 0 to 1
 * @arms: how dendritic a flake is, 0 to 1.  0 is a plain hexagonal
 *   plate, 1 is a stellar dendrite with side branches on every arm
 * @drift: the steady sideways wind, in pixels per fall
 * @flutter: how far a falling flake wanders sideways, as a fraction of a
 *   column
 * @spin: how fast a falling flake turns, in turns per fall
 * @melt: where in a settled flake's life the melt begins, 0 to 1.  0.45
 *   is a flake that sits for a while first; 0 is a warm pane
 * @shrink: how much smaller the water bead is than the flake it came
 *   from.  A snowflake is mostly air, so this is severe on purpose:
 *   0.35 is about right and 1.0 is a flake that turns into a puddle its
 *   own size
 * @bulge: how domed the melt-water bead is
 * @depth: how far the wallpaper is behind the pane, in bead radii ---
 *   the rain's meaning exactly, because a melted flake IS a rain drop
 * @dispersion: chromatic separation inside a bead
 * @runs: how many columns have melt-water running down them, 0 to 1
 * @run_width: pixels per column of the melt-water layer
 * @run_len: pixels of trail behind a running bead
 * @beads: how much of a trail is left behind as residual drops
 * @frost: how much frost grows in from the edges of the pane, 0 to 1
 * @frost_scale: pixels per feather of that frost
 * @sparkle: how much a crystal glitters, 0 to 1
 * @fog: how frosted the bare pane is, 0 to 1
 * @clarity: how much of that a water bead lifts
 * @glow: how much brighter a dry crystal is than the pane.  Snow does
 *   not refract, it SCATTERS: a crystal is a bright diffusing patch, and
 *   drawing it as a lens is the single most common way to get snow wrong
 * @specular: strength of the glint on a water bead
 * @shine: specular exponent
 * @rim: how much darker the edge of a bead is than its middle
 * @light: direction to the light, as a 3-vector
 * @tint: what the melt-water takes out of the light
 * @absorption: how much of @tint is applied
 * @brightness: multiplied into the result
 * @alpha: overall opacity
 * @src_origin: where this rect's top-left sits in the source textures
 * @src_scale: how many SOURCE pixels one pixel of this rect is
 * @seed: which crop of the snow this rect shows
 *
 * One window in a snowfall.  gowl_fx_snow_params_init() fills in a
 * steady fall on a pane just above freezing.
 */
typedef struct {
	gint   width, height;
	gfloat radius;
	gfloat flake;
	gfloat cell;
	gfloat settled;
	gfloat column;
	gfloat falling;
	gfloat arms;
	gfloat drift;
	gfloat flutter;
	gfloat spin;
	gfloat melt;
	gfloat shrink;
	gfloat bulge;
	gfloat depth;
	gfloat dispersion;
	gfloat runs;
	gfloat run_width;
	gfloat run_len;
	gfloat beads;
	gfloat frost;
	gfloat frost_scale;
	gfloat sparkle;
	gfloat fog;
	gfloat clarity;
	gfloat glow;
	gfloat specular;
	gfloat shine;
	gfloat rim;
	gfloat light[3];
	gfloat tint[3];
	gfloat absorption;
	gfloat brightness;
	gfloat alpha;
	gfloat src_origin[2];
	gfloat src_scale;
	gfloat seed;
} GowlFxSnowParams;

/**
 * gowl_fx_snow_params_init:
 * @params: (out): the parameters to reset
 */
void gowl_fx_snow_params_init (GowlFxSnowParams *params);

/**
 * gowl_fx_pass_snow:
 * @pass: a pass, begun on the buffer the snow is drawn into
 * @soft: the frosted wallpaper, covering the whole output
 * @sharp: (nullable): the same wallpaper unblurred
 * @params: the snowfall to draw
 * @clock: (nullable): where the snow has got to; %NULL is a warm pane
 *
 * Draws snow falling past a window, settling on it and melting off it.
 *
 * A falling flake is a six-fold crystal that turns as it comes down and
 * wanders on the wind.  A flake that lands sits as a bright scattering
 * crystal, rounds off as it melts, collapses to a water bead a third its
 * size --- which is a rain drop, and refracts like one --- and runs away
 * down the pane leaving a beaded trail.  Frost creeps in from the edges
 * while it happens.
 *
 * Like the water and the rain, this is NEVER up to date.
 *
 * Returns: %FALSE when the shader could not be built, which is not an
 *   error --- the caller shows no snow and the desktop is as it was.
 */
gboolean gowl_fx_pass_snow (GowlFxPass             *pass,
                            const GowlFxTexture    *soft,
                            const GowlFxTexture    *sharp,
                            const GowlFxSnowParams *params,
                            const GowlFxSnowClock  *clock);

/* ── PQ output encode ────────────────────────────────────────────── */

/**
 * GOWL_FX_PQ_SDR_WHITE:
 *
 * Where SDR diffuse white sits in an HDR signal, in cd/m2.
 *
 * 203, from ITU-R BT.2408.  It is not a preference: PQ code values are
 * absolute luminance, so an SDR desktop has to be told where its own
 * white belongs, and 203 is the number the rest of the industry grades
 * and masters against.
 */
#define GOWL_FX_PQ_SDR_WHITE (203.0)

/**
 * gowl_fx_pass_pq:
 * @pass: a pass, begun on the buffer that will be committed to the output
 * @scene: the composited desktop, sRGB-encoded, as a texture
 * @sdr_white: where SDR white should land, in cd/m2; 0 means
 *   %GOWL_FX_PQ_SDR_WHITE
 * @peak: the panel's own peak luminance in cd/m2, from its EDID; 0 means
 *   a conventional 1000
 *
 * Encodes an SDR desktop for an output being driven in BT.2020 and PQ:
 * sRGB EOTF, BT.709 to BT.2020 primaries, scaled so white lands on
 * @sdr_white and clamped to @peak, then the inverse PQ EOTF.
 *
 * This is what wlroots does in its renderer and only under Vulkan.  On
 * the GLES2 renderer gowl needs for its effects, without it, an HDR
 * output receives sRGB code values with PQ's meaning -- so white asks
 * the panel for 10,000 cd/m2 instead of 203, the backlight runs at its
 * peak, and any client that DOES encode correctly is the only correctly
 * scaled window on the screen.
 *
 * An OUTPUT transform only: it states an all-SDR desktop correctly in an
 * HDR signal, and gives a client no way to deliver PQ content of its
 * own.
 *
 * Returns: %FALSE when the shader could not be built, which is not an
 *   error --- the caller commits the scene unencoded, as it did before
 */
gboolean gowl_fx_pass_pq (GowlFxPass          *pass,
                          const GowlFxTexture *scene,
                          gdouble              sdr_white,
                          gdouble              peak);

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
/**
 * gowl_fx_vis_hide_sheets:
 * @vis: a #GowlFxVis
 *
 * Hides every effect sheet parked in the scene.  Hiding the client
 * layers does not cover these -- a sheet hangs off scene->tree beside
 * them -- and a sheet holds a picture of the desktop WITH its windows,
 * so any capture that means to see past the windows must call this too.
 */
void gowl_fx_vis_hide_sheets (GowlFxVis *vis);

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
 *
 * The returned buffer belongs to the OUTPUT'S SWAPCHAIN.  Present it and
 * drop it within the frame; never store it.
 *
 * Holding one keeps a slot permanently out of the rotation the output
 * needs to present, and ties the content to a pool the compositor keeps
 * drawing the live desktop into.  If that slot is ever handed back out
 * -- a swapchain recreated on a mode or format change, a lock dropped,
 * a scanout path that does not consult the lock -- what you are holding
 * silently stops being your capture and becomes a photograph of the
 * desktop, windows and all.
 *
 * Anything that needs a captured image to outlive the frame must
 * allocate its own buffer, as #GowlFxSheet and the blur backdrop do
 * with their own wlr_swapchain.
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
 * @GOWL_FX_SHEET_ABOVE_OVERLAY: park above every layer but the session
 *   lock.  Nothing is left above it, so unlike the other placements this
 *   hides NOTHING --- which matters for an effect that captures the
 *   screen every frame, because a client the sheet had switched off to
 *   get out of its way would be missing from that capture too
 * @GOWL_FX_SHEET_FILTER: this sheet is a filter over the finished
 *   screen rather than a picture of some other state, so no capture
 *   should ever see it.  gowl_fx_capture() hides these for the length of
 *   its render: without it the tube photographs its own last frame, and
 *   a magnifier or a cube would photograph the tube and put a second
 *   one through it
 */
typedef enum {
	GOWL_FX_SHEET_NONE            = 0,
	GOWL_FX_SHEET_ABOVE_TOP       = 1 << 0,
	GOWL_FX_SHEET_KEEP_FULLSCREEN = 1 << 1,
	GOWL_FX_SHEET_ABOVE_OVERLAY   = 1 << 2,
	GOWL_FX_SHEET_FILTER          = 1 << 3
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
