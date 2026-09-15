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

#include "gowl-config.h"
#include "boxed/gowl-palette.h"
#include "gowl-keybind.h"
#include "gowl-enums.h"
#include "gowl-types.h"

#include <glib.h>
#include <glib-object.h>
#include <string.h>
#include <linux/input-event-codes.h>

/* yaml-glib headers -- available via -Ideps/yaml-glib/src */
#include "yaml-glib.h"

/* --- Default values --- */
#define GOWL_CONFIG_DEFAULT_BORDER_WIDTH        (2)
/*
 * The defaults are palette names, not literals.  A hex default
 * would be a fourth independent palette that no theme change can
 * reach --- which is exactly the state this replaced.
 */
#define GOWL_CONFIG_DEFAULT_BORDER_COLOR_FOCUS   "accent"
#define GOWL_CONFIG_DEFAULT_BORDER_COLOR_UNFOCUS "surface"
#define GOWL_CONFIG_DEFAULT_BORDER_COLOR_URGENT  "red"
#define GOWL_CONFIG_DEFAULT_PALETTE              "mocha"
#define GOWL_CONFIG_DEFAULT_MFACT               (0.55)
/* Two columns visible at a time, matching Omarchy's Hyprland default. */
#define GOWL_CONFIG_DEFAULT_SCROLL_COLUMN_WIDTH (0.5)
/* Layout motion settles quickly; entrances get their own longer beat. */
#define GOWL_CONFIG_DEFAULT_ANIMATION_DURATION  (260)
/*
 * -1 means "use animation-duration".  Opening is an arrival rather
 * than a correction, so it can afford a longer beat than a re-tile.
 */
#define GOWL_CONFIG_DEFAULT_ANIMATION_DURATION_OPEN (360)
/* Shorter than either: a closed window is finished, and holding its
 * ghost on screen is holding up the re-tile behind it. */
#define GOWL_CONFIG_DEFAULT_ANIMATION_DURATION_CLOSE (180)
#define GOWL_CONFIG_DEFAULT_ANIMATION_CURVE     "ease-out-expo"
#define GOWL_CONFIG_DEFAULT_ANIMATION_CURVE_OPEN "ease-out-back"
#define GOWL_CONFIG_DEFAULT_ANIMATION_CURVE_CLOSE "almost-linear"
#define GOWL_CONFIG_DEFAULT_ANIMATION_POPIN_SCALE (0.84)

/* Desktop cube.  260 ms of animation is right for a window sliding a few
 * hundred pixels; a whole desktop turning through 90 degrees needs longer
 * or it reads as a cut.  The step add-on is deliberately much less than
 * the base: three faces should take about twice one face, not three times. */
#define GOWL_CONFIG_DEFAULT_CUBE_DURATION       (520)
#define GOWL_CONFIG_DEFAULT_CUBE_STEP_DURATION  (150)
#define GOWL_CONFIG_DEFAULT_CUBE_CURVE          "ease-in-out-cubic"
#define GOWL_CONFIG_DEFAULT_CUBE_FACES          (4)
#define GOWL_CONFIG_DEFAULT_CUBE_ZOOM           (1.45)
#define GOWL_CONFIG_DEFAULT_CUBE_PITCH          (14.0)
#define GOWL_CONFIG_DEFAULT_CUBE_SHADING        (0.78)
#define GOWL_CONFIG_DEFAULT_CUBE_REFLECTION     (0.32)
#define GOWL_CONFIG_DEFAULT_CUBE_MOTION_BLUR    (0.35)
#define GOWL_CONFIG_DEFAULT_CUBE_BACKDROP_COLOR "#12141f"

#define GOWL_CONFIG_DEFAULT_MAGNIFIER_MAX        (8.0)
#define GOWL_CONFIG_DEFAULT_MAGNIFIER_STEP       (1.25)
#define GOWL_CONFIG_DEFAULT_MAGNIFIER_SMOOTHING  (120)
#define GOWL_CONFIG_DEFAULT_MAGNIFIER_MODIFIER   "Super"

#define GOWL_CONFIG_DEFAULT_EXPO_DURATION        (340)
#define GOWL_CONFIG_DEFAULT_EXPO_CURVE           "ease-out-expo"
#define GOWL_CONFIG_DEFAULT_EXPO_TAGS            (9)
#define GOWL_CONFIG_DEFAULT_EXPO_GAP             (0.06)
#define GOWL_CONFIG_DEFAULT_EXPO_CORNER          (0.035)
#define GOWL_CONFIG_DEFAULT_EXPO_DIM             (0.45)
#define GOWL_CONFIG_DEFAULT_EXPO_BACKDROP_COLOR  "#12141f"

#define GOWL_CONFIG_DEFAULT_SWITCHER_DURATION    (220)
#define GOWL_CONFIG_DEFAULT_SWITCHER_CURVE       "ease-out-expo"
#define GOWL_CONFIG_DEFAULT_SWITCHER_SCALE       (0.52)
#define GOWL_CONFIG_DEFAULT_SWITCHER_SPACING     (0.62)
#define GOWL_CONFIG_DEFAULT_SWITCHER_ANGLE       (52.0)
#define GOWL_CONFIG_DEFAULT_SWITCHER_REFLECTION  (0.28)
#define GOWL_CONFIG_DEFAULT_SWITCHER_BACKDROP_COLOR "#0e1018"

#define GOWL_CONFIG_DEFAULT_BLUR_DOWNSCALE       (4)
#define GOWL_CONFIG_DEFAULT_BLUR_PASSES          (3)
#define GOWL_CONFIG_DEFAULT_BLUR_BRIGHTNESS      (0.9)
#define GOWL_CONFIG_DEFAULT_SHADOW_RADIUS        (28)
#define GOWL_CONFIG_DEFAULT_SHADOW_OPACITY       (0.42)
#define GOWL_CONFIG_DEFAULT_SHADOW_OFFSET_X      (0)
#define GOWL_CONFIG_DEFAULT_SHADOW_OFFSET_Y      (10)
#define GOWL_CONFIG_DEFAULT_SHADOW_COLOR         "#000000"

/*
 * The liquid-glass defaults: a narrow bevel over a very thick slab with a
 * FOLDING slope (above 1), so the flat centre stays clear while the rim
 * concentrates the wallpaper into a band that shows it twice.  They are
 * proportions of a window, in logical pixels, and the module scales them
 * by the output's scale.
 *
 * The optical ratios are hyalite's, tuned by eye by its author.  The
 * three geometric ones are not: hyalite tunes for web elements a few
 * hundred pixels across, and its 37-pixel bevel over a 59-pixel slab is a
 * proportion of a button, not of a window.  On a window it draws a hairline
 * nobody would call glass.  Scaled up to a bevel of 56 over a slab of 110
 * the bevel is the same SHARE of the thing it edges, which is what rule 3
 * of hyalite's own list asks for -- geometry scales, optics does not, and
 * dispersion, the rim window and the specular line below stay exactly
 * where they were tuned.
 *
 * The three that are NOT proportions -- dispersion, the edge window and
 * the rim -- are fixed pixel counts on purpose.  A colour fringe and the
 * hairline under it are one or two pixels of real glass whatever size the
 * window is; scaling them with the bevel is what turns a wide rim into
 * grey mud.
 */
#define GOWL_CONFIG_DEFAULT_GLASS_BEVEL          (56.0)
#define GOWL_CONFIG_DEFAULT_GLASS_THICKNESS      (110.0)
#define GOWL_CONFIG_DEFAULT_GLASS_SLOPE          (3.0)
#define GOWL_CONFIG_DEFAULT_GLASS_SHAPE          "squircle"
#define GOWL_CONFIG_DEFAULT_GLASS_DISPERSION     (1.6)
#define GOWL_CONFIG_DEFAULT_GLASS_RIM            (1.76)
#define GOWL_CONFIG_DEFAULT_GLASS_SHADE          (0.46)
#define GOWL_CONFIG_DEFAULT_GLASS_EDGE_WIDTH     (8.0)
#define GOWL_CONFIG_DEFAULT_GLASS_SATURATION     (0.86)
#define GOWL_CONFIG_DEFAULT_GLASS_CLARITY        (0.85)
#define GOWL_CONFIG_DEFAULT_GLASS_CENTRE_CLARITY (0.45)
#define GOWL_CONFIG_DEFAULT_GLASS_LENS           (0.055)
#define GOWL_CONFIG_DEFAULT_GLASS_SHEEN          (0.30)
#define GOWL_CONFIG_DEFAULT_GLASS_LIGHT          (-140.0)
#define GOWL_CONFIG_DEFAULT_GLASS_TINT           "#ffffff"
#define GOWL_CONFIG_DEFAULT_GLASS_BRIGHTNESS     (1.0)
#define GOWL_CONFIG_DEFAULT_GLASS_OPACITY        (1.0)
#define GOWL_CONFIG_DEFAULT_GLASS_FROST          (3)
#define GOWL_CONFIG_DEFAULT_GLASS_FROST_PASSES   (2)

/*
 * The liquid-water defaults.
 *
 * `water-preset' does the real work: each name is a whole tuned set, and
 * the five of them span what the effect is FOR --- a barely-disturbed
 * pool, a fountain's ripples, a pond, an open sea, a storm.  The
 * individual keys below are overrides on top of whichever preset is
 * selected, and start at a sentinel meaning "the preset decides", so a
 * config that names a preset and nothing else gets all of it.
 *
 * Amplitude is a share of wavelength in disguise: what the refraction
 * sees is the slope, and the displacement is depth * (1 - 1/1.333) *
 * slope.  That is why the numbers in the presets run to hundreds of
 * pixels of `depth' --- a first attempt with a 26-pixel depth worked out
 * to under half a pixel of displacement, which is to say no refraction at
 * all.
 */
#define GOWL_CONFIG_DEFAULT_WATER_PRESET         "sea"
#define GOWL_CONFIG_DEFAULT_WATER_INTENSITY      (0.4)
#define GOWL_CONFIG_DEFAULT_WATER_FPS            (30)
#define GOWL_CONFIG_DEFAULT_WATER_SCALE          (2)
#define GOWL_CONFIG_DEFAULT_WATER_TINT           "#9edbff"
#define GOWL_CONFIG_DEFAULT_WATER_CLARITY        (0.45)
#define GOWL_CONFIG_DEFAULT_WATER_OPACITY        (1.0)
#define GOWL_CONFIG_DEFAULT_WATER_BRIGHTNESS     (1.0)
#define GOWL_CONFIG_DEFAULT_WATER_LIGHT          (-140.0)
#define GOWL_CONFIG_DEFAULT_WATER_FROST          (3)
#define GOWL_CONFIG_DEFAULT_WATER_FROST_PASSES   (2)
/* "the preset decides", for every override that is a length or a weight.
 * Negative is impossible for all of them, which is what makes it usable
 * as a sentinel without a second `has-' flag per key. */
#define GOWL_CONFIG_WATER_FROM_PRESET            (-1.0)

/*
 * The liquid-rain defaults.
 *
 * Same shape as the water above -- a named preset carries a whole tuned
 * set and the individual keys are overrides on top of it -- because the
 * numbers here mean even less on their own than the water's do.  Drop
 * size, how many cells hold a drop, how far the refracted ray travels
 * and how long a drop lives are one description of weather between them;
 * a downpour's density over a mist's cell size is not heavier rain, it
 * is a window someone has sprayed.
 *
 * `rain-cell' is the ruler.  A drop is between a tenth and a third of a
 * cell across and most cells are empty, so a 46 px cell a third full is
 * the scatter a window picks up in a shower.
 */
/*
 * Bokeh: the blur module's other kernel.
 *
 * A 34-pixel disc through a six-bladed aperture, which is a fast lens
 * stopped down a little -- the aperture reads as a hexagon around any
 * bright thing on the wallpaper and as a circle everywhere else, which
 * is what a photograph looks like.
 *
 * `bokeh-highlight' at 5 is the number that matters and the one nobody
 * would guess.  A wallpaper has no values above 1.0, so a lens's real
 * dynamic range -- a highlight hundreds of times brighter than the wall
 * behind it -- has to be put back by hand or the discs are drawn and
 * invisible.  At 0 this is a box blur with hard edges.
 */
#define GOWL_CONFIG_DEFAULT_BOKEH_RADIUS         (34.0)
#define GOWL_CONFIG_DEFAULT_BOKEH_DOWNSCALE      (2)
/*
 * The taps, and this is the expensive knob.
 *
 * Two separate things go wrong when there are too few, and they look
 * nothing like each other.
 *
 * A disc of radius R sampled N times has its taps about R/sqrt(N/pi)
 * apart, so a highlight SMALLER than that spacing lands on some taps and
 * between others, and what gets drawn is a RING rather than a filled
 * shape.  That one is fixed by a few dozen taps.
 *
 * The other is GRAIN, and it needs far more.  The highlight weighting is
 * what makes the discs visible at all, and it also means a handful of
 * bright samples carry most of the weight -- so the effective sample
 * count is a fraction of the nominal one, and a detailed wallpaper comes
 * out speckled.  128 is where that stops being visible on a photograph;
 * 48 is plainly grainy and 96 is still slightly so.
 *
 * It is paid ONCE PER OUTPUT PER TAG SWITCH and never per frame, which
 * is the only reason a number this large is reasonable.  Lower it if a
 * tag switch hitches; raise `bokeh-radius' and this wants raising too.
 */
#define GOWL_CONFIG_DEFAULT_BOKEH_SAMPLES        (128)
#define GOWL_CONFIG_DEFAULT_BOKEH_BLADES         (6)
#define GOWL_CONFIG_DEFAULT_BOKEH_ROTATION       (12.0)
#define GOWL_CONFIG_DEFAULT_BOKEH_HIGHLIGHT      (5.0)
#define GOWL_CONFIG_DEFAULT_BOKEH_THRESHOLD      (0.55)
#define GOWL_CONFIG_DEFAULT_BOKEH_EDGE           (0.22)
#define GOWL_CONFIG_DEFAULT_BOKEH_BRIGHTNESS     (1.0)


/*
 * The four newest backdrops: the soap film, the embers, the submerged
 * view and the dew.
 *
 * Every one of them follows the shape the rain, the fizz, the leaves and
 * the snow settled on, because the shape works:
 *
 *   A PRESET IS A WHOLE TUNED SET and not a starting point.  The numbers
 *   in one only mean anything together -- a fire's column width against
 *   its ember size is how many sparks there are, and either alone is a
 *   different fire in a different sized window.  `<fx>-preset' picks
 *   one; every other key is an OVERRIDE on it, and leaving a key out is
 *   not the same as setting it to a default.
 *
 *   ONE INTENSITY KNOB scales the two or three numbers that together
 *   mean "how much of this is happening", and deliberately not the
 *   sizes.  Scaling the sizes does not give a busier effect; it gives
 *   the same effect in a smaller window.
 *
 *   THE SENTINEL IS NEGATIVE, because every one of these is a length, a
 *   fraction, a count or a temperature and none of them can be.
 */

#define GOWL_CONFIG_DEFAULT_SOAP_PRESET  "drifting"
#define GOWL_CONFIG_DEFAULT_SOAP_INTENSITY   (1.0)
#define GOWL_CONFIG_DEFAULT_SOAP_FPS         (30)
#define GOWL_CONFIG_DEFAULT_SOAP_SCALE       (2)
#define GOWL_CONFIG_DEFAULT_SOAP_TINT  "#ffffff"
#define GOWL_CONFIG_DEFAULT_SOAP_CLARITY     (0.55)
#define GOWL_CONFIG_DEFAULT_SOAP_OPACITY     (1.0)
#define GOWL_CONFIG_DEFAULT_SOAP_BRIGHTNESS  (1.0)
#define GOWL_CONFIG_DEFAULT_SOAP_LIGHT       (-140.0)
#define GOWL_CONFIG_DEFAULT_SOAP_FROST       (3)
#define GOWL_CONFIG_DEFAULT_SOAP_FROST_PASSES (2)
/* "leave it to the preset", for every override key below. */
#define GOWL_CONFIG_SOAP_FROM_PRESET         (-1.0)

#define GOWL_CONFIG_DEFAULT_EMBERS_PRESET  "embers"
#define GOWL_CONFIG_DEFAULT_EMBERS_INTENSITY   (1.0)
#define GOWL_CONFIG_DEFAULT_EMBERS_FPS         (30)
#define GOWL_CONFIG_DEFAULT_EMBERS_SCALE       (2)
#define GOWL_CONFIG_DEFAULT_EMBERS_TINT  "#ffdcb8"
#define GOWL_CONFIG_DEFAULT_EMBERS_CLARITY     (0.55)
#define GOWL_CONFIG_DEFAULT_EMBERS_OPACITY     (1.0)
#define GOWL_CONFIG_DEFAULT_EMBERS_BRIGHTNESS  (1.0)
#define GOWL_CONFIG_DEFAULT_EMBERS_FROST       (3)
#define GOWL_CONFIG_DEFAULT_EMBERS_FROST_PASSES (2)
/* "leave it to the preset", for every override key below. */
#define GOWL_CONFIG_EMBERS_FROM_PRESET         (-1.0)

#define GOWL_CONFIG_DEFAULT_SUBMERGED_PRESET  "reef"
#define GOWL_CONFIG_DEFAULT_SUBMERGED_INTENSITY   (1.0)
#define GOWL_CONFIG_DEFAULT_SUBMERGED_FPS         (30)
#define GOWL_CONFIG_DEFAULT_SUBMERGED_SCALE       (2)
#define GOWL_CONFIG_DEFAULT_SUBMERGED_WATER  "#0b4761"
#define GOWL_CONFIG_DEFAULT_SUBMERGED_CLARITY     (0.55)
#define GOWL_CONFIG_DEFAULT_SUBMERGED_OPACITY     (1.0)
#define GOWL_CONFIG_DEFAULT_SUBMERGED_BRIGHTNESS  (1.0)
#define GOWL_CONFIG_DEFAULT_SUBMERGED_FROST       (3)
#define GOWL_CONFIG_DEFAULT_SUBMERGED_FROST_PASSES (2)
/* "leave it to the preset", for every override key below. */
#define GOWL_CONFIG_SUBMERGED_FROM_PRESET         (-1.0)

#define GOWL_CONFIG_DEFAULT_DEW_PRESET  "dawn"
#define GOWL_CONFIG_DEFAULT_DEW_INTENSITY   (1.0)
#define GOWL_CONFIG_DEFAULT_DEW_FPS         (30)
#define GOWL_CONFIG_DEFAULT_DEW_SCALE       (2)
#define GOWL_CONFIG_DEFAULT_DEW_TINT  "#f5fbff"
#define GOWL_CONFIG_DEFAULT_DEW_CLARITY     (0.55)
#define GOWL_CONFIG_DEFAULT_DEW_OPACITY     (1.0)
#define GOWL_CONFIG_DEFAULT_DEW_BRIGHTNESS  (1.0)
#define GOWL_CONFIG_DEFAULT_DEW_LIGHT       (-140.0)
#define GOWL_CONFIG_DEFAULT_DEW_FROST       (3)
#define GOWL_CONFIG_DEFAULT_DEW_FROST_PASSES (2)
/* "leave it to the preset", for every override key below. */
#define GOWL_CONFIG_DEW_FROM_PRESET         (-1.0)


#define GOWL_CONFIG_DEFAULT_RAIN_PRESET          "shower"
#define GOWL_CONFIG_DEFAULT_RAIN_INTENSITY       (1.0)
/*
 * The storm.
 *
 * Off for `rain' and forced on for `storm', which is the same effect
 * and the same preset with this switched.
 *
 * Nine seconds between strikes is a storm a mile or two off rather than
 * overhead: close enough to be a storm, far enough that the flash is an
 * event instead of a strobe.  The gaps are exponential around that mean,
 * so some come almost together and some leave a long wait -- which is
 * most of what makes it read as weather rather than as a timer.
 */
#define GOWL_CONFIG_DEFAULT_RAIN_LIGHTNING       (FALSE)
#define GOWL_CONFIG_DEFAULT_RAIN_LIGHTNING_RATE  (9.0)
#define GOWL_CONFIG_DEFAULT_RAIN_LIGHTNING_POWER (1.0)
#define GOWL_CONFIG_DEFAULT_RAIN_FPS             (30)
#define GOWL_CONFIG_DEFAULT_RAIN_SCALE           (2)
#define GOWL_CONFIG_DEFAULT_RAIN_TINT            "#dceeff"
#define GOWL_CONFIG_DEFAULT_RAIN_CLARITY         (0.95)
#define GOWL_CONFIG_DEFAULT_RAIN_OPACITY         (1.0)
#define GOWL_CONFIG_DEFAULT_RAIN_BRIGHTNESS      (1.0)
#define GOWL_CONFIG_DEFAULT_RAIN_LIGHT           (-140.0)
#define GOWL_CONFIG_DEFAULT_RAIN_FROST           (3)
#define GOWL_CONFIG_DEFAULT_RAIN_FROST_PASSES    (2)
#define GOWL_CONFIG_RAIN_FROM_PRESET             (-1.0)

/*
 * The carbonation defaults.
 *
 * Same shape as the water and the rain above -- a named preset carries a
 * whole tuned set and the individual keys are overrides on top of it --
 * and here the presets are literally a scale, which is what the effect
 * is for: `sparkling' barely moves, `soda' is a glass of cola, `seltzer'
 * is a hard-carbonated water that will not sit still.  `seltzer' is the
 * shipped default rather than the middle one, because the effect is
 * worth turning on for the bubbles and a cola's worth of them reads as
 * a slightly grubby pane until you know what you are looking at.
 *
 * `fizz-site-width' is the ruler, not the cell.  What the eye counts in
 * a fizzy drink is TRAINS -- files of bubbles streaming from one point
 * on the glass -- so the number of nucleation sites across the window is
 * the number that means "how carbonated is this", and the bubble size
 * follows from it.  Raising the bubble size without narrowing the
 * columns gives bigger bubbles rather than more of them, which is a
 * different drink and usually a worse one.
 */
#define GOWL_CONFIG_DEFAULT_FIZZ_PRESET          "seltzer"
#define GOWL_CONFIG_DEFAULT_FIZZ_INTENSITY       (1.0)
#define GOWL_CONFIG_DEFAULT_FIZZ_FPS             (30)
#define GOWL_CONFIG_DEFAULT_FIZZ_SCALE           (2)
#define GOWL_CONFIG_DEFAULT_FIZZ_TINT            "#fff5db"
#define GOWL_CONFIG_DEFAULT_FIZZ_CLARITY         (0.90)
#define GOWL_CONFIG_DEFAULT_FIZZ_OPACITY         (1.0)
#define GOWL_CONFIG_DEFAULT_FIZZ_BRIGHTNESS      (1.0)
#define GOWL_CONFIG_DEFAULT_FIZZ_LIGHT           (-140.0)
#define GOWL_CONFIG_DEFAULT_FIZZ_FROST           (3)
#define GOWL_CONFIG_DEFAULT_FIZZ_FROST_PASSES    (2)
#define GOWL_CONFIG_FIZZ_FROM_PRESET             (-1.0)

/*
 * The falling-leaves defaults.
 *
 * `leaves-preset' spans a season rather than a storm: `turning' is the
 * first few coming down, `autumn' is the middle of it, `peak' is the
 * week the tree empties, and the two windy ones are the same fall with
 * the weather against it.
 *
 * `leaves-leaf' is the ruler and it is capped against BOTH the column
 * and the cell at render time, because a leaf wider than a third of its
 * own column would be sliced off at the column edge.  So asking for
 * bigger leaves without widening the spacing quietly gets smaller ones;
 * the presets move the three together, which is the whole reason they
 * are a table.
 */
#define GOWL_CONFIG_DEFAULT_LEAVES_PRESET        "autumn"
#define GOWL_CONFIG_DEFAULT_LEAVES_INTENSITY     (1.0)
#define GOWL_CONFIG_DEFAULT_LEAVES_FPS           (30)
#define GOWL_CONFIG_DEFAULT_LEAVES_SCALE         (2)
#define GOWL_CONFIG_DEFAULT_LEAVES_WARM          "#dc381f"
#define GOWL_CONFIG_DEFAULT_LEAVES_GOLD          "#f29e22"
#define GOWL_CONFIG_DEFAULT_LEAVES_DRY           "#a3622b"
#define GOWL_CONFIG_DEFAULT_LEAVES_OPACITY       (1.0)
#define GOWL_CONFIG_DEFAULT_LEAVES_BRIGHTNESS    (1.0)
#define GOWL_CONFIG_DEFAULT_LEAVES_LIGHT         (-140.0)
#define GOWL_CONFIG_DEFAULT_LEAVES_FROST         (3)
#define GOWL_CONFIG_DEFAULT_LEAVES_FROST_PASSES  (2)
#define GOWL_CONFIG_LEAVES_FROM_PRESET           (-1.0)

/*
 * The snow defaults.
 *
 * `snow-preset' is a scale of how hard it is coming down, and -- unlike
 * the rain's, which only ever gets wetter -- it is also a scale of
 * TEMPERATURE.  A flurry is a warm pane: what lands melts almost at
 * once and runs.  A blizzard is a cold one: it settles, it stays, and
 * the frost grows in from the edges while it does.  `snow-melt' and
 * `snow-frost-rate' are what carry that, and they move opposite ways
 * across the table.
 *
 * `snow-flake' is the ruler.  It is small compared with the rain's cell
 * on purpose: a crystal has to be big enough for its six arms to be
 * legible, and a window of flakes too small to resolve is dust.
 */
#define GOWL_CONFIG_DEFAULT_SNOW_PRESET          "steady"
#define GOWL_CONFIG_DEFAULT_SNOW_INTENSITY       (1.0)
#define GOWL_CONFIG_DEFAULT_SNOW_FPS             (30)
#define GOWL_CONFIG_DEFAULT_SNOW_SCALE           (2)
#define GOWL_CONFIG_DEFAULT_SNOW_TINT            "#e0f0ff"
#define GOWL_CONFIG_DEFAULT_SNOW_CLARITY         (0.92)
#define GOWL_CONFIG_DEFAULT_SNOW_OPACITY         (1.0)
#define GOWL_CONFIG_DEFAULT_SNOW_BRIGHTNESS      (1.0)
#define GOWL_CONFIG_DEFAULT_SNOW_LIGHT           (-140.0)
#define GOWL_CONFIG_DEFAULT_SNOW_FROST           (3)
#define GOWL_CONFIG_DEFAULT_SNOW_FROST_PASSES    (2)
#define GOWL_CONFIG_SNOW_FROM_PRESET             (-1.0)

#define GOWL_CONFIG_DEFAULT_WALLPAPER_FADE       (320)

/*
 * The window-hint overlay.
 *
 * `hints-keys' is the home row first, then the top row, then the bottom
 * -- not alphabetical, because the first eight windows should be
 * reachable without moving a finger.  It is also what decides whether
 * labels are one character or two: the overlay uses one while the
 * alphabet is long enough for the windows on screen and two once it is
 * not, so a longer alphabet is how somebody with a great many windows
 * keeps single-key hints.
 *
 * `hints-timeout' is 0, which is where this parts company with tmux.
 * `display-panes-time' there is one second and is the most complained
 * about thing about the feature: the labels go away while you are still
 * looking for the one you want.  Waiting costs nothing, because every
 * key is swallowed while the overlay is up and Escape is right there.
 *
 * `hints-warp-pointer' is on because `sloppyfocus' is: focusing a window
 * across the desk and leaving the cursor behind means the next nudge of
 * the mouse hands focus straight back.
 */
#define GOWL_CONFIG_DEFAULT_HINTS_KEYS \
	"asdfghjklqwertyuiopzxcvbnm"
/* Palette NAMES, so the overlay follows whatever flavour is configured
 * rather than hard-coding one.  Ordered for contrast between consecutive
 * entries, since telling one badge from the badge next to it is the
 * whole job. */
#define GOWL_CONFIG_DEFAULT_HINTS_COLORS \
	"mauve,green,peach,blue,pink,teal,yellow,red,sapphire,lavender," \
	"flamingo,sky"
#define GOWL_CONFIG_DEFAULT_HINTS_TIMEOUT        (0)
#define GOWL_CONFIG_DEFAULT_HINTS_SIZE           (96)
#define GOWL_CONFIG_DEFAULT_HINTS_BORDER_WIDTH   (3)
#define GOWL_CONFIG_DEFAULT_HINTS_SCRIM          (0.40)
#define GOWL_CONFIG_DEFAULT_HINTS_WARP_POINTER   (TRUE)
#define GOWL_CONFIG_DEFAULT_HINTS_CURRENT_OUTPUT (FALSE)
/*
 * Windows that get no effects at all.
 *
 * ADDED to the built-in `steam', `steamwebhelper' and `mutter-devkit',
 * never replacing them -- see src/util/gowl-fx-optout.c.  Empty by
 * default because the built-ins already cover the two things anybody
 * asked for, and because the other way of saying it, `GOWL_NO_FX=1' in
 * a process's environment, needs no config at all.
 */
#define GOWL_CONFIG_DEFAULT_NO_FX_APPS          ""
#define GOWL_CONFIG_DEFAULT_NMASTER             (1)
#define GOWL_CONFIG_DEFAULT_TAG_COUNT           (9)
#define GOWL_CONFIG_DEFAULT_REPEAT_RATE         (25)
#define GOWL_CONFIG_DEFAULT_REPEAT_DELAY        (600)
#define GOWL_CONFIG_DEFAULT_TERMINAL            "gst"
#define GOWL_CONFIG_DEFAULT_MENU                "bemenu-run"
#define GOWL_CONFIG_DEFAULT_SLOPPYFOCUS         (TRUE)
#define GOWL_CONFIG_DEFAULT_MANAGE_LID          (TRUE)
#define GOWL_CONFIG_DEFAULT_IDLE_TIMEOUT        (300)
#define GOWL_CONFIG_DEFAULT_XKB_LAYOUT          (NULL)
#define GOWL_CONFIG_DEFAULT_DPMS_TIMEOUT        (0)
#define GOWL_CONFIG_DEFAULT_ALLOW_TEARING       (FALSE)
/*
 * TWO decisions, and they were one key to begin with, which was wrong.
 *
 * `hdr-unmanaged' is whether HDR may be switched ON where the compositor
 * cannot convert colour for it.  TRUE: the KMS half of HDR works -- the
 * panel really does go into PQ -- and what is missing is the conversion
 * of SDR content into it, which is a thing to be told about rather than
 * prevented from having.  gowl says so in the log and in the toast.
 *
 * `hdr-advertise-pq' is whether to also tell CLIENTS that PQ and BT.2020
 * content is accepted.  FALSE, and that is the one that has to stay off
 * by default: a client that believes it encodes itself correctly for the
 * output -- Chromium does -- and is then the only correctly scaled thing
 * on a screen where everything else is being passed through at the
 * panel's peak.  Which is not a dim client, it is a bright everything
 * else, and it was reported as "Electron goes dark in HDR".
 *
 * Turn it on for a player that encodes PQ itself, and accept that it
 * will look darker than the desktop around it.
 */
#define GOWL_CONFIG_DEFAULT_HDR_UNMANAGED       (TRUE)
#define GOWL_CONFIG_DEFAULT_HDR_ADVERTISE_PQ    (FALSE)
/*
 * Where SDR diffuse white lands in the HDR signal, in cd/m2.
 *
 * 203 is ITU-R BT.2408's reference white, which is what the rest of the
 * industry grades and masters against, and it is the number that decides
 * whether an HDR desktop idles or runs the panel at its peak.  It is a
 * key rather than a constant because it is also the one honest brightness
 * control an HDR output has: the backlight does nothing there, so this is
 * what "make the desktop dimmer" means.
 */
#define GOWL_CONFIG_DEFAULT_HDR_SDR_WHITE       (203.0)
/*
 * Whether gowl encodes the desktop for PQ itself, and at what depth.
 *
 * Both exist to be turned OFF, which is the point of them.  An HDR
 * output that comes out wrong has two candidate causes that look nothing
 * alike in the code and identical on the glass -- the encode getting it
 * wrong, and the panel making a mess of a 10-bit link it nominally
 * accepted -- and without a way to take each out of the picture there is
 * no way to find out which.
 */
#define GOWL_CONFIG_DEFAULT_HDR_ENCODE          (TRUE)
/* 0 asks for ten bits and settles for eight; 8 never asks. */
#define GOWL_CONFIG_DEFAULT_HDR_BPC             (0)
#define GOWL_CONFIG_DEFAULT_FOCUS_ON_ACTIVATE   ("smart")
#define GOWL_CONFIG_DEFAULT_INPUT_RECORDING     (FALSE)
#define GOWL_CONFIG_DEFAULT_INPUT_RECORDING_DENY_APPS ""
#define GOWL_CONFIG_DEFAULT_LOG_LEVEL           "warning"
#define GOWL_CONFIG_DEFAULT_LOG_FILE            "~/.config/gowl/gowl.log"
#define GOWL_CONFIG_DEFAULT_EVALUATE_GOWL_CONFIG_WITH_CMACS  (TRUE)
#define GOWL_CONFIG_DEFAULT_EVALUATE_C_CONFIG_WITH_CMACS     (TRUE)

/* Configuration file name */
#define GOWL_CONFIG_FILENAME "config.yaml"

static void gowl_config_reresolve_colors(GowlConfig *self);
static GEnumClass *backdrop_enum_class(void);

/* --- Instance struct --- */

struct _GowlConfig {
	GObject parent_instance;

	/* Appearance */
	gint     border_width;
	gchar   *border_color_focus;
	gchar   *border_color_unfocus;
	gchar   *border_color_urgent;

	/*
	 * Colour specs above are stored exactly as written, so that
	 * to_yaml round-trips a palette reference rather than baking it
	 * into a literal the next theme change cannot reach.  The
	 * resolved forms are shadows, recomputed whenever either the
	 * spec or the palette changes, because every consumer wants a
	 * hex string and none of them should have to know about
	 * palettes.
	 */
	gchar   *border_hex_focus;
	gchar   *border_hex_unfocus;
	gchar   *border_hex_urgent;

	/* Palette */
	GowlPalette *palette;             /* effective, after merging */
	GowlPalette *palette_override;    /* pushed in at runtime */
	gchar       *palette_name;

	/* Layout */
	gint     animation_duration_open;
	gint     animation_duration_close;
	gdouble  mfact;
	gdouble  scroll_column_width;
	gboolean animations;
	gint     animation_duration;
	gchar   *animation_curve;
	gchar   *animation_curve_open;
	gchar   *animation_curve_close;
	gdouble  animation_popin_scale;
	gdouble  animation_jiggle_strength;

	gboolean cube;
	gint     cube_duration;
	gint     cube_step_duration;
	gchar   *cube_curve;
	gint     cube_faces;
	gdouble  cube_zoom;
	gdouble  cube_pitch;
	gdouble  cube_shading;
	gdouble  cube_reflection;
	gdouble  cube_motion_blur;
	gchar   *cube_backdrop_color;
	gboolean cube_caps;
	gboolean cube_all_monitors;
	gboolean cube_gesture;

	gboolean magnifier;
	gdouble  magnifier_max;
	gdouble  magnifier_step;
	gint     magnifier_smoothing;
	gboolean magnifier_follow_cursor;
	gboolean magnifier_smooth;
	gchar   *magnifier_modifier;

	gboolean expo;
	gint     expo_duration;
	gchar   *expo_curve;
	gint     expo_tags;
	gint     expo_columns;
	gdouble  expo_gap;
	gdouble  expo_corner;
	gdouble  expo_dim;
	gboolean expo_hide_empty;
	gchar   *expo_backdrop_color;

	gboolean switcher;
	gint     switcher_duration;
	gchar   *switcher_curve;
	gdouble  switcher_scale;
	gdouble  switcher_spacing;
	gdouble  switcher_angle;
	gdouble  switcher_reflection;
	gboolean switcher_all_tags;
	gchar   *switcher_backdrop_color;

	gboolean blur;
	gint     blur_downscale;
	gint     blur_passes;
	gdouble  blur_brightness;
	gboolean shadow;
	gint     shadow_radius;
	gdouble  shadow_opacity;
	gint     shadow_offset_x;
	gint     shadow_offset_y;
	gchar   *shadow_color;
	gint     backdrop_style;      /* GowlBackdropStyle */
	gdouble  glass_bevel;
	gdouble  glass_thickness;
	gdouble  glass_slope;
	gchar   *glass_shape;
	gdouble  glass_dispersion;
	gdouble  glass_rim;
	gdouble  glass_shade;
	gdouble  glass_edge_width;
	gdouble  glass_saturation;
	gdouble  glass_clarity;
	gdouble  glass_centre_clarity;
	gdouble  glass_lens;
	gdouble  glass_sheen;
	gdouble  glass_light;
	gchar   *glass_tint;
	gdouble  glass_brightness;
	gdouble  glass_opacity;
	gint     glass_frost;
	gint     glass_frost_passes;
	gchar   *water_preset;
	gdouble  water_intensity;
	gint     water_fps;
	gint     water_scale;
	gchar   *water_tint;
	gdouble  water_clarity;
	gdouble  water_opacity;
	gdouble  water_brightness;
	gdouble  water_light;
	gint     water_frost;
	gint     water_frost_passes;
	/* Overrides on the preset; GOWL_CONFIG_WATER_FROM_PRESET for "leave
	 * it alone". */
	gdouble  water_amplitude;
	gdouble  water_wavelength;
	gdouble  water_choppiness;
	gdouble  water_depth;
	gdouble  water_drops;
	gdouble  water_shore;
	gdouble  water_specular;
	gdouble  water_caustics;
	gdouble  water_foam;
	gdouble  water_fresnel;
	gdouble  water_speed;
	gchar   *rain_preset;
	gdouble  rain_intensity;
	gint     rain_fps;
	gint     rain_scale;
	gchar   *rain_tint;
	gdouble  rain_clarity;
	gdouble  rain_opacity;
	gdouble  rain_brightness;
	gdouble  rain_light;
	gint     rain_frost;
	gint     rain_frost_passes;
	/* Overrides on the preset; GOWL_CONFIG_RAIN_FROM_PRESET for "leave
	 * it alone". */
	gdouble  rain_cell;
	gdouble  rain_density;
	gdouble  rain_bulge;
	gdouble  rain_depth;
	gdouble  rain_runs;
	gdouble  rain_run_width;
	gdouble  rain_run_length;
	gdouble  rain_beads;
	gdouble  rain_fog;
	gdouble  rain_specular;
	gchar   *soap_preset;
	gdouble  soap_intensity;
	gint     soap_fps;
	gint     soap_scale;
	gchar   *soap_tint;
	gdouble  soap_clarity;
	gdouble  soap_opacity;
	gdouble  soap_brightness;
	gdouble  soap_light;
	gint     soap_frost;
	gint     soap_frost_passes;
	/* Overrides on the preset; GOWL_CONFIG_SOAP_FROM_PRESET for "leave
	 * it alone". */
	gdouble  soap_thickness;
	gdouble  soap_thin;
	gdouble  soap_drain;
	gdouble  soap_turbulence;
	gdouble  soap_swirl;
	gdouble  soap_index;
	gdouble  soap_gain;
	gdouble  soap_sheen;
	gdouble  soap_wedge;
	gdouble  soap_pop;
	gdouble  soap_meniscus;
	gdouble  soap_fog;
	gdouble  soap_speed;
	gdouble  soap_life;

	gchar   *embers_preset;
	gdouble  embers_intensity;
	gint     embers_fps;
	gint     embers_scale;
	gchar   *embers_tint;
	gdouble  embers_clarity;
	gdouble  embers_opacity;
	gdouble  embers_brightness;
	gint     embers_frost;
	gint     embers_frost_passes;
	/* Overrides on the preset; GOWL_CONFIG_EMBERS_FROM_PRESET for "leave
	 * it alone". */
	gdouble  embers_column;
	gdouble  embers_density;
	gdouble  embers_ember;
	gdouble  embers_spacing;
	gdouble  embers_sway;
	gdouble  embers_drag;
	gdouble  embers_temperature;
	gdouble  embers_cool;
	gdouble  embers_flicker;
	gdouble  embers_ash;
	gdouble  embers_glow;
	gdouble  embers_hearth;
	gdouble  embers_haze;
	gdouble  embers_haze_scale;
	gdouble  embers_fog;
	gdouble  embers_speed;

	gchar   *submerged_preset;
	gdouble  submerged_intensity;
	gint     submerged_fps;
	gint     submerged_scale;
	gchar   *submerged_water;
	gdouble  submerged_clarity;
	gdouble  submerged_opacity;
	gdouble  submerged_brightness;
	gint     submerged_frost;
	gint     submerged_frost_passes;
	/* Overrides on the preset; GOWL_CONFIG_SUBMERGED_FROM_PRESET for "leave
	 * it alone". */
	gdouble  submerged_depth;
	gdouble  submerged_extinction_r;
	gdouble  submerged_extinction_g;
	gdouble  submerged_extinction_b;
	gdouble  submerged_murk;
	gdouble  submerged_caustics;
	gdouble  submerged_caustic_scale;
	gdouble  submerged_shafts;
	gdouble  submerged_shaft_lean;
	gdouble  submerged_motes;
	gdouble  submerged_mote_size;
	gdouble  submerged_surface;
	gdouble  submerged_sway;
	gdouble  submerged_fog;
	gdouble  submerged_speed;
	gdouble  submerged_drift;

	gchar   *dew_preset;
	gdouble  dew_intensity;
	gint     dew_fps;
	gint     dew_scale;
	gchar   *dew_tint;
	gdouble  dew_clarity;
	gdouble  dew_opacity;
	gdouble  dew_brightness;
	gdouble  dew_light;
	gint     dew_frost;
	gint     dew_frost_passes;
	/* Overrides on the preset; GOWL_CONFIG_DEW_FROM_PRESET for "leave
	 * it alone". */
	gdouble  dew_radials;
	gdouble  dew_pitch;
	gdouble  dew_thread;
	gdouble  dew_drop;
	gdouble  dew_spacing;
	gdouble  dew_sag;
	gdouble  dew_depth;
	gdouble  dew_bulge;
	gdouble  dew_silk;
	gdouble  dew_glint;
	gdouble  dew_shine;
	gdouble  dew_rim;
	gdouble  dew_sway;
	gdouble  dew_fog;
	gdouble  dew_speed;


	gdouble  bokeh_radius;
	gint     bokeh_downscale;
	gint     bokeh_samples;
	gint     bokeh_blades;
	gdouble  bokeh_rotation;
	gdouble  bokeh_highlight;
	gdouble  bokeh_threshold;
	gdouble  bokeh_edge;
	gdouble  bokeh_brightness;

	gdouble  rain_impact;
	gdouble  rain_speed;
	gboolean rain_lightning;
	gdouble  rain_lightning_rate;
	gdouble  rain_lightning_power;

	gchar   *fizz_preset;
	gdouble  fizz_intensity;
	gint     fizz_fps;
	gint     fizz_scale;
	gchar   *fizz_tint;
	gdouble  fizz_clarity;
	gdouble  fizz_opacity;
	gdouble  fizz_brightness;
	gdouble  fizz_light;
	gint     fizz_frost;
	gint     fizz_frost_passes;
	/* Overrides on the preset; GOWL_CONFIG_FIZZ_FROM_PRESET for "leave
	 * it alone". */
	gdouble  fizz_cell;
	gdouble  fizz_bubble;
	gdouble  fizz_growth;
	gdouble  fizz_sites;
	gdouble  fizz_site_width;
	gdouble  fizz_spacing;
	gdouble  fizz_stray;
	gdouble  fizz_cling;
	gdouble  fizz_wobble;
	gdouble  fizz_foam;
	gdouble  fizz_foam_depth;
	gdouble  fizz_depth;
	gdouble  fizz_mirror;
	gdouble  fizz_fog;
	gdouble  fizz_specular;
	gdouble  fizz_speed;

	gchar   *leaves_preset;
	gdouble  leaves_intensity;
	gint     leaves_fps;
	gint     leaves_scale;
	gchar   *leaves_warm;
	gchar   *leaves_gold;
	gchar   *leaves_dry;
	gdouble  leaves_opacity;
	gdouble  leaves_brightness;
	gdouble  leaves_light;
	gint     leaves_frost;
	gint     leaves_frost_passes;
	/* Overrides on the preset; GOWL_CONFIG_LEAVES_FROM_PRESET for
	 * "leave it alone". */
	gdouble  leaves_leaf;
	gdouble  leaves_cell;
	gdouble  leaves_stuck;
	gdouble  leaves_column;
	gdouble  leaves_falling;
	gdouble  leaves_flutter;
	gdouble  leaves_tumble;
	gdouble  leaves_wind;
	gdouble  leaves_gust;
	gdouble  leaves_gustiness;
	gdouble  leaves_curl;
	gdouble  leaves_veins;
	gdouble  leaves_translucency;
	gdouble  leaves_gloss;
	gdouble  leaves_shadow;
	gdouble  leaves_fog;
	gdouble  leaves_speed;
	gdouble  leaves_tenure;

	gchar   *snow_preset;
	gdouble  snow_intensity;
	gint     snow_fps;
	gint     snow_scale;
	gchar   *snow_tint;
	gdouble  snow_clarity;
	gdouble  snow_opacity;
	gdouble  snow_brightness;
	gdouble  snow_light;
	gint     snow_frost;
	gint     snow_frost_passes;
	/* Overrides on the preset; GOWL_CONFIG_SNOW_FROM_PRESET for "leave
	 * it alone". */
	gdouble  snow_flake;
	gdouble  snow_cell;
	gdouble  snow_settled;
	gdouble  snow_column;
	gdouble  snow_falling;
	gdouble  snow_arms;
	gdouble  snow_drift;
	gdouble  snow_flutter;
	gdouble  snow_spin;
	gdouble  snow_melt;
	gdouble  snow_shrink;
	gdouble  snow_depth;
	gdouble  snow_runs;
	gdouble  snow_run_width;
	gdouble  snow_run_length;
	gdouble  snow_beads;
	gdouble  snow_ice;
	gdouble  snow_ice_rate;
	gdouble  snow_ice_scale;
	gdouble  snow_sparkle;
	gdouble  snow_fog;
	gdouble  snow_glow;
	gdouble  snow_specular;
	gdouble  snow_speed;

	gchar   *hints_keys;
	gchar   *hints_colors;
	gint     hints_timeout;
	gint     hints_size;
	gint     hints_border_width;
	gdouble  hints_scrim;
	gboolean hints_warp_pointer;
	gboolean hints_current_output;

	/* Comma-separated app_ids / process names that get no effects,
	 * on top of the built-in list. */
	gchar   *no_fx_apps;

	/* Per-tag wallpaper overrides, 1-based; NULL means "use the default
	 * wallpaper", which is what every entry is until a config says
	 * otherwise. */
	gchar   *wallpaper_tags[GOWL_CONFIG_MAX_TAGS];
	/* Per-output wallpaper overrides: an output key (the same keys
	 * `monitors:' takes -- connector name, "Make Model", "Make Model
	 * Serial" or "*") mapped to a #GowlWallpaperOutput.  This is what
	 * makes a 21:9 desk monitor and a 16:9 laptop panel each show a
	 * picture drawn for their own shape instead of one of them showing
	 * a centre-crop of the other's.  NULL until a config declares any. */
	GHashTable *wallpaper_outputs;
	gint     wallpaper_fade;
	gint     nmaster;
	gint     tag_count;

	/* Input */
	gint     repeat_rate;
	gint     repeat_delay;
	gboolean sloppyfocus;
	gboolean manage_lid;
	/* What locks the screen, and whether suspending does it.  See the
	 * header. */
	gchar   *lock_command;
	gboolean lock_on_suspend;
	gchar   *xkb_layout;
	gchar   *xkb_variant;
	gchar   *xkb_model;
	gchar   *xkb_options;
	gchar   *xkb_rules;
	gchar   *xkb_file;
	gint     idle_timeout;
	gint     dpms_timeout;
	gboolean allow_tearing;
	gboolean hdr_unmanaged;
	gboolean hdr_advertise_pq;
	gdouble  hdr_sdr_white;
	gboolean hdr_encode;
	gint     hdr_bpc;
	gchar   *focus_on_activate;
	gboolean input_recording;
	gchar   *input_recording_deny_apps;

	/* Programs */
	gchar   *terminal;
	gchar   *menu;

	/* Logging */
	gchar   *log_level;
	gchar   *log_file;

	/* cmacs evaluation gates (root-level in YAML / C config).
	 * Only consulted by cmacs `--gowl` startup logic; gowl's
	 * standalone main.c never reads these fields. */
	gboolean evaluate_gowl_config_with_cmacs;
	gboolean evaluate_c_config_with_cmacs;

	/* Keybinds - array of GowlKeybindEntry */
	GArray  *keybinds;

	/* Pointer binds - array of GowlMousebindEntry */
	GArray  *mousebinds;

	/* Gesture binds - array of GowlGestureEntry */
	GArray  *gestures;

	/* input: blocks - array of GowlInputConfigEntry* (heap) */
	GPtrArray *input_configs;

	/* Rules - array of GowlRuleEntry* (heap-allocated) */
	GPtrArray *rules;

	/* Dropdowns - array of GowlDropdownEntry* (heap-allocated) */
	GPtrArray *dropdowns;

	/* Module configs - maps module name (gchar*) to per-module
	 * GHashTable<gchar*, gchar*> of key-value settings parsed
	 * from the YAML modules section. */
	GHashTable *module_configs;

	/* Problems the last YAML load found (unknown keys, bad values) */
	guint problems;

	/* Monitor configs - maps output name (gchar*) to a heap-allocated
	 * GowlMonitorConfig* parsed from the YAML monitors: section.
	 * Each field is independently optional (sentinel-driven). */
	GHashTable *monitor_configs;

	/* Output profiles from `profiles:`, in file order */
	GList *profiles;
};

G_DEFINE_FINAL_TYPE(GowlConfig, gowl_config, G_TYPE_OBJECT)

static void output_profile_free(gpointer data);
static gint gowl_parse_monitor_transform(YamlMapping *cm);

/* --- Signal IDs --- */
enum {
	SIGNAL_CHANGED,
	SIGNAL_RELOADED,
	N_SIGNALS
};

static guint signals[N_SIGNALS] = { 0 };

/* --- GObject property storage --- */
static GParamSpec *properties[GOWL_CONFIG_PROP_LAST] = { NULL };

/* --- Helper: free a GowlRuleEntry --- */

/**
 * gowl_rule_entry_free:
 * @entry: a heap-allocated #GowlRuleEntry
 *
 * Frees all strings inside the rule entry and then the entry itself.
 */
static void
gowl_rule_entry_free(gpointer entry)
{
	GowlRuleEntry *r = (GowlRuleEntry *)entry;

	if (r == NULL)
		return;
	g_free(r->app_id);
	g_free(r->title);
	g_free(r->initial_title);
	g_free(r);
}

/**
 * gowl_dropdown_entry_free:
 * @entry: a heap-allocated #GowlDropdownEntry
 *
 * Frees the strings owned by the entry and then the entry.
 */
static void
gowl_dropdown_entry_free(gpointer entry)
{
	GowlDropdownEntry *d = (GowlDropdownEntry *)entry;

	if (d == NULL)
		return;
	g_free(d->name);
	g_free(d->spawn_cmd);
	g_free(d->keybind);
	g_free(d);
}

/* --- Helper: escape a string for a YAML double-quoted scalar --- */

/**
 * gowl_config_escape_yaml:
 * @str: (nullable): the string to escape
 *
 * Escapes @str for inclusion inside a YAML double-quoted scalar.
 * A double-quoted scalar uses JSON-style backslash escapes, so a
 * literal backslash or quote in a spawn command (a regex in a rule,
 * a shell argument) must be doubled or the emitted document does not
 * parse.  Control characters are escaped as \xNN.
 *
 * Deliberately NOT g_strescape(): that emits octal escapes for bytes
 * >= 0x80, which YAML does not define, so any non-ASCII description
 * would come back as literal backslash-digits.  UTF-8 is passed
 * through untouched instead.
 *
 * Returns: (transfer full): a newly allocated escaped string
 */
static gchar *
gowl_config_escape_yaml(const gchar *str)
{
	GString *out;
	const gchar *p;

	if (str == NULL)
		return g_strdup("");

	out = g_string_sized_new(strlen(str) + 8);

	for (p = str; *p != '\0'; p++) {
		guchar c = (guchar)*p;

		switch (c) {
		case '"':  g_string_append(out, "\\\""); break;
		case '\\': g_string_append(out, "\\\\"); break;
		case '\n': g_string_append(out, "\\n");  break;
		case '\r': g_string_append(out, "\\r");  break;
		case '\t': g_string_append(out, "\\t");  break;
		default:
			/* Escape the remaining C0 controls; pass every other
			 * byte (UTF-8 continuation bytes included) through. */
			if (c < 0x20 || c == 0x7f)
				g_string_append_printf(out, "\\x%02x", c);
			else
				g_string_append_c(out, (gchar)c);
			break;
		}
	}

	return g_string_free(out, FALSE);
}

/* --- Helper: free a GowlKeybindEntry (array element) --- */

/* Clear funcs for the pointer bind, gesture and input arrays. */
static void
gowl_mousebind_entry_clear(gpointer entry)
{
	GowlMousebindEntry *mb = (GowlMousebindEntry *)entry;

	g_clear_pointer(&mb->arg, g_free);
	g_clear_pointer(&mb->desc, g_free);
}

static void
gowl_gesture_entry_clear(gpointer entry)
{
	GowlGestureEntry *ge = (GowlGestureEntry *)entry;

	g_clear_pointer(&ge->arg, g_free);
	g_clear_pointer(&ge->desc, g_free);
}

static void
gowl_input_config_entry_free(gpointer entry)
{
	GowlInputConfigEntry *ic = (GowlInputConfigEntry *)entry;

	g_free(ic->match);
	g_clear_pointer(&ic->settings, g_hash_table_unref);
	g_free(ic);
}

/* The two grabs gowl always had.  In the array so that a config can
 * see, replace or remove them like any other bind. */
static void
gowl_config_add_default_mousebinds(GowlConfig *self)
{
	gowl_config_add_mousebind(self, GOWL_KEY_MOD_LOGO, BTN_LEFT,
	                          GOWL_ACTION_MOVE_WINDOW, NULL, "Move window");
	gowl_config_add_mousebind(self, GOWL_KEY_MOD_LOGO, BTN_RIGHT,
	                          GOWL_ACTION_RESIZE_WINDOW, NULL, "Resize window");
}

/**
 * gowl_keybind_entry_clear:
 * @entry: pointer to a #GowlKeybindEntry stored in a GArray
 *
 * Frees the owned strings inside the keybind entry.
 */
static void
gowl_keybind_entry_clear(gpointer entry)
{
	GowlKeybindEntry *kb = (GowlKeybindEntry *)entry;

	g_clear_pointer(&kb->mode, g_free);

	g_clear_pointer(&kb->arg, g_free);
	g_clear_pointer(&kb->desc, g_free);
}

/* --- GObject vfuncs --- */

/**
 * gowl_config_set_property:
 *
 * GObject set_property vfunc for #GowlConfig.
 * Sets each GObject property and emits the "changed" signal.
 */
static void
gowl_config_set_property(
	GObject      *object,
	guint         prop_id,
	const GValue *value,
	GParamSpec   *pspec
){
	GowlConfig *self = GOWL_CONFIG(object);

	switch ((GowlConfigProp)prop_id) {
	case GOWL_CONFIG_PROP_BORDER_WIDTH:
		self->border_width = g_value_get_int(value);
		break;
	case GOWL_CONFIG_PROP_BORDER_COLOR_FOCUS:
		g_free(self->border_color_focus);
		self->border_color_focus = g_value_dup_string(value);
		gowl_config_reresolve_colors(self);
		break;
	case GOWL_CONFIG_PROP_BORDER_COLOR_UNFOCUS:
		g_free(self->border_color_unfocus);
		self->border_color_unfocus = g_value_dup_string(value);
		gowl_config_reresolve_colors(self);
		break;
	case GOWL_CONFIG_PROP_BORDER_COLOR_URGENT:
		g_free(self->border_color_urgent);
		self->border_color_urgent = g_value_dup_string(value);
		gowl_config_reresolve_colors(self);
		break;
	case GOWL_CONFIG_PROP_MFACT:
		self->mfact = g_value_get_double(value);
		break;
	case GOWL_CONFIG_PROP_NMASTER:
		self->nmaster = g_value_get_int(value);
		break;
	case GOWL_CONFIG_PROP_TAG_COUNT:
		self->tag_count = g_value_get_int(value);
		break;
	case GOWL_CONFIG_PROP_REPEAT_RATE:
		self->repeat_rate = g_value_get_int(value);
		break;
	case GOWL_CONFIG_PROP_REPEAT_DELAY:
		self->repeat_delay = g_value_get_int(value);
		break;
	case GOWL_CONFIG_PROP_TERMINAL:
		g_free(self->terminal);
		self->terminal = g_value_dup_string(value);
		break;
	case GOWL_CONFIG_PROP_MENU:
		g_free(self->menu);
		self->menu = g_value_dup_string(value);
		break;
	case GOWL_CONFIG_PROP_SLOPPYFOCUS:
		self->sloppyfocus = g_value_get_boolean(value);
		break;
	case GOWL_CONFIG_PROP_MANAGE_LID:
		self->manage_lid = g_value_get_boolean(value);
		break;
	case GOWL_CONFIG_PROP_XKB_LAYOUT:
		g_free(self->xkb_layout);
		self->xkb_layout = g_value_dup_string(value);
		break;
	case GOWL_CONFIG_PROP_XKB_VARIANT:
		g_free(self->xkb_variant);
		self->xkb_variant = g_value_dup_string(value);
		break;
	case GOWL_CONFIG_PROP_XKB_MODEL:
		g_free(self->xkb_model);
		self->xkb_model = g_value_dup_string(value);
		break;
	case GOWL_CONFIG_PROP_XKB_OPTIONS:
		g_free(self->xkb_options);
		self->xkb_options = g_value_dup_string(value);
		break;
	case GOWL_CONFIG_PROP_XKB_RULES:
		g_free(self->xkb_rules);
		self->xkb_rules = g_value_dup_string(value);
		break;
	case GOWL_CONFIG_PROP_XKB_FILE:
		g_free(self->xkb_file);
		self->xkb_file = g_value_dup_string(value);
		break;
	case GOWL_CONFIG_PROP_IDLE_TIMEOUT:
		self->idle_timeout = g_value_get_int(value);
		break;
	case GOWL_CONFIG_PROP_DPMS_TIMEOUT:
		self->dpms_timeout = g_value_get_int(value);
		break;
	case GOWL_CONFIG_PROP_ALLOW_TEARING:
		self->allow_tearing = g_value_get_boolean(value);
		break;
	case GOWL_CONFIG_PROP_HDR_ENCODE:
		self->hdr_encode = g_value_get_boolean(value);
		break;
	case GOWL_CONFIG_PROP_HDR_BPC:
		{
			gint v = g_value_get_int(value);

			self->hdr_bpc = (v == 8 || v == 10) ? v : 0;
		}
		break;
	case GOWL_CONFIG_PROP_HDR_SDR_WHITE:
		self->hdr_sdr_white = CLAMP(g_value_get_double(value), 40.0, 600.0);
		break;
	case GOWL_CONFIG_PROP_HDR_UNMANAGED:
		self->hdr_unmanaged = g_value_get_boolean(value);
		break;
	case GOWL_CONFIG_PROP_HDR_ADVERTISE_PQ:
		self->hdr_advertise_pq = g_value_get_boolean(value);
		break;
	case GOWL_CONFIG_PROP_FOCUS_ON_ACTIVATE:
		g_free(self->focus_on_activate);
		self->focus_on_activate = g_value_dup_string(value);
		break;
	case GOWL_CONFIG_PROP_INPUT_RECORDING:
		self->input_recording = g_value_get_boolean(value);
		break;
	case GOWL_CONFIG_PROP_NO_FX_APPS:
		g_free(self->no_fx_apps);
		self->no_fx_apps = g_value_dup_string(value);
		if (self->no_fx_apps == NULL)
			self->no_fx_apps = g_strdup(GOWL_CONFIG_DEFAULT_NO_FX_APPS);
		break;
	case GOWL_CONFIG_PROP_INPUT_RECORDING_DENY_APPS:
		g_free(self->input_recording_deny_apps);
		self->input_recording_deny_apps = g_value_dup_string(value);
		break;
	case GOWL_CONFIG_PROP_LOG_LEVEL:
		g_free(self->log_level);
		self->log_level = g_value_dup_string(value);
		break;
	case GOWL_CONFIG_PROP_LOG_FILE:
		g_free(self->log_file);
		self->log_file = g_value_dup_string(value);
		break;
	case GOWL_CONFIG_PROP_EVALUATE_GOWL_CONFIG_WITH_CMACS:
		self->evaluate_gowl_config_with_cmacs = g_value_get_boolean(value);
		break;
	case GOWL_CONFIG_PROP_EVALUATE_C_CONFIG_WITH_CMACS:
		self->evaluate_c_config_with_cmacs = g_value_get_boolean(value);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
		return;
	}

	/* Emit "changed" signal with the property name */
	g_signal_emit(self, signals[SIGNAL_CHANGED], 0, pspec->name);
}

/**
 * gowl_config_get_property:
 *
 * GObject get_property vfunc for #GowlConfig.
 */
static void
gowl_config_get_property(
	GObject    *object,
	guint       prop_id,
	GValue     *value,
	GParamSpec *pspec
){
	GowlConfig *self = GOWL_CONFIG(object);

	switch ((GowlConfigProp)prop_id) {
	case GOWL_CONFIG_PROP_BORDER_WIDTH:
		g_value_set_int(value, self->border_width);
		break;
	case GOWL_CONFIG_PROP_BORDER_COLOR_FOCUS:
		g_value_set_string(value, self->border_color_focus);
		break;
	case GOWL_CONFIG_PROP_BORDER_COLOR_UNFOCUS:
		g_value_set_string(value, self->border_color_unfocus);
		break;
	case GOWL_CONFIG_PROP_BORDER_COLOR_URGENT:
		g_value_set_string(value, self->border_color_urgent);
		break;
	case GOWL_CONFIG_PROP_MFACT:
		g_value_set_double(value, self->mfact);
		break;
	case GOWL_CONFIG_PROP_NMASTER:
		g_value_set_int(value, self->nmaster);
		break;
	case GOWL_CONFIG_PROP_TAG_COUNT:
		g_value_set_int(value, self->tag_count);
		break;
	case GOWL_CONFIG_PROP_REPEAT_RATE:
		g_value_set_int(value, self->repeat_rate);
		break;
	case GOWL_CONFIG_PROP_REPEAT_DELAY:
		g_value_set_int(value, self->repeat_delay);
		break;
	case GOWL_CONFIG_PROP_TERMINAL:
		g_value_set_string(value, self->terminal);
		break;
	case GOWL_CONFIG_PROP_MENU:
		g_value_set_string(value, self->menu);
		break;
	case GOWL_CONFIG_PROP_SLOPPYFOCUS:
		g_value_set_boolean(value, self->sloppyfocus);
		break;
	case GOWL_CONFIG_PROP_MANAGE_LID:
		g_value_set_boolean(value, self->manage_lid);
		break;
	case GOWL_CONFIG_PROP_XKB_LAYOUT:
		g_value_set_string(value, self->xkb_layout);
		break;
	case GOWL_CONFIG_PROP_XKB_VARIANT:
		g_value_set_string(value, self->xkb_variant);
		break;
	case GOWL_CONFIG_PROP_XKB_MODEL:
		g_value_set_string(value, self->xkb_model);
		break;
	case GOWL_CONFIG_PROP_XKB_OPTIONS:
		g_value_set_string(value, self->xkb_options);
		break;
	case GOWL_CONFIG_PROP_XKB_RULES:
		g_value_set_string(value, self->xkb_rules);
		break;
	case GOWL_CONFIG_PROP_XKB_FILE:
		g_value_set_string(value, self->xkb_file);
		break;
	case GOWL_CONFIG_PROP_IDLE_TIMEOUT:
		g_value_set_int(value, self->idle_timeout);
		break;
	case GOWL_CONFIG_PROP_DPMS_TIMEOUT:
		g_value_set_int(value, self->dpms_timeout);
		break;
	case GOWL_CONFIG_PROP_ALLOW_TEARING:
		g_value_set_boolean(value, self->allow_tearing);
		break;
	case GOWL_CONFIG_PROP_HDR_ENCODE:
		g_value_set_boolean(value, self->hdr_encode);
		break;
	case GOWL_CONFIG_PROP_HDR_BPC:
		g_value_set_int(value, self->hdr_bpc);
		break;
	case GOWL_CONFIG_PROP_HDR_SDR_WHITE:
		g_value_set_double(value, self->hdr_sdr_white);
		break;
	case GOWL_CONFIG_PROP_HDR_UNMANAGED:
		g_value_set_boolean(value, self->hdr_unmanaged);
		break;
	case GOWL_CONFIG_PROP_HDR_ADVERTISE_PQ:
		g_value_set_boolean(value, self->hdr_advertise_pq);
		break;
	case GOWL_CONFIG_PROP_FOCUS_ON_ACTIVATE:
		g_value_set_string(value, self->focus_on_activate);
		break;
	case GOWL_CONFIG_PROP_INPUT_RECORDING:
		g_value_set_boolean(value, self->input_recording);
		break;
	case GOWL_CONFIG_PROP_NO_FX_APPS:
		g_value_set_string(value, self->no_fx_apps);
		break;
	case GOWL_CONFIG_PROP_INPUT_RECORDING_DENY_APPS:
		g_value_set_string(value, self->input_recording_deny_apps);
		break;
	case GOWL_CONFIG_PROP_LOG_LEVEL:
		g_value_set_string(value, self->log_level);
		break;
	case GOWL_CONFIG_PROP_LOG_FILE:
		g_value_set_string(value, self->log_file);
		break;
	case GOWL_CONFIG_PROP_EVALUATE_GOWL_CONFIG_WITH_CMACS:
		g_value_set_boolean(value, self->evaluate_gowl_config_with_cmacs);
		break;
	case GOWL_CONFIG_PROP_EVALUATE_C_CONFIG_WITH_CMACS:
		g_value_set_boolean(value, self->evaluate_c_config_with_cmacs);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
		break;
	}
}

/**
 * gowl_config_finalize:
 *
 * Releases all resources owned by the #GowlConfig instance.
 */
/*
 * Recomputes every resolved colour shadow from its spec.  Called after
 * anything that can change a spec or the palette; cheap enough (three
 * hash lookups) that no attempt is made to work out which ones
 * actually changed.
 */
static void
gowl_config_reresolve_colors(GowlConfig *self)
{
	g_free(self->border_hex_focus);
	g_free(self->border_hex_unfocus);
	g_free(self->border_hex_urgent);

	self->border_hex_focus = gowl_palette_resolve(self->palette,
	                                              self->border_color_focus);
	self->border_hex_unfocus = gowl_palette_resolve(self->palette,
	                                                self->border_color_unfocus);
	self->border_hex_urgent = gowl_palette_resolve(self->palette,
	                                               self->border_color_urgent);
}

/*
 * Rebuilds the effective palette: the named built-in, then whatever the
 * config file defined on top of it, then whatever was pushed in at
 * runtime.  Layered in that order so a reload cannot discard a palette
 * an editor theme pushed in --- the file is the base, the override is
 * the last word.
 */
static void
gowl_config_rebuild_palette(GowlConfig *self, const GowlPalette *from_file)
{
	g_clear_pointer(&self->palette, gowl_palette_free);
	self->palette = gowl_palette_new_builtin(self->palette_name);
	gowl_palette_merge(self->palette, from_file);
	gowl_palette_merge(self->palette, self->palette_override);
	gowl_config_reresolve_colors(self);
}

static void
gowl_config_finalize(GObject *object)
{
	GowlConfig *self = GOWL_CONFIG(object);

	g_free(self->border_color_focus);
	g_free(self->border_color_unfocus);
	g_free(self->border_color_urgent);
	g_free(self->border_hex_focus);
	g_free(self->border_hex_unfocus);
	g_free(self->border_hex_urgent);
	g_free(self->palette_name);
	g_clear_pointer(&self->palette, gowl_palette_free);
	g_clear_pointer(&self->palette_override, gowl_palette_free);
	g_free(self->terminal);
	g_free(self->menu);
	g_free(self->log_level);
	g_free(self->log_file);
	g_free(self->input_recording_deny_apps);
	g_free(self->focus_on_activate);
	g_free(self->animation_curve);
	g_free(self->animation_curve_open);
	g_free(self->animation_curve_close);
	g_free(self->cube_curve);
	g_free(self->cube_backdrop_color);
	g_free(self->magnifier_modifier);
	g_free(self->expo_curve);
	g_free(self->expo_backdrop_color);
	g_free(self->switcher_curve);
	g_free(self->switcher_backdrop_color);
	g_free(self->shadow_color);
	g_free(self->glass_shape);
	g_free(self->glass_tint);
	g_free(self->water_preset);
	g_free(self->water_tint);
	g_free(self->rain_preset);
	g_free(self->rain_tint);
	g_free(self->fizz_preset);
	g_free(self->fizz_tint);
	g_free(self->leaves_preset);
	g_free(self->leaves_warm);
	g_free(self->leaves_gold);
	g_free(self->leaves_dry);
	g_free(self->snow_preset);
	g_free(self->snow_tint);
	g_free(self->soap_preset);
	g_free(self->soap_tint);

	g_free(self->embers_preset);
	g_free(self->embers_tint);

	g_free(self->submerged_preset);
	g_free(self->submerged_water);

	g_free(self->dew_preset);
	g_free(self->dew_tint);


	g_free(self->hints_keys);
	g_free(self->hints_colors);
	g_free(self->no_fx_apps);
	{
		gint ti;

		for (ti = 0; ti < GOWL_CONFIG_MAX_TAGS; ti++)
			g_free(self->wallpaper_tags[ti]);
	}
	g_clear_pointer(&self->wallpaper_outputs, g_hash_table_unref);
	g_free(self->lock_command);

	if (self->keybinds != NULL)
		g_array_unref(self->keybinds);
	if (self->mousebinds != NULL)
		g_array_unref(self->mousebinds);
	if (self->gestures != NULL)
		g_array_unref(self->gestures);
	g_clear_pointer(&self->input_configs, g_ptr_array_unref);
	g_free(self->xkb_layout);
	g_free(self->xkb_variant);
	g_free(self->xkb_model);
	g_free(self->xkb_options);
	g_free(self->xkb_rules);
	g_free(self->xkb_file);
	if (self->rules != NULL)
		g_ptr_array_unref(self->rules);
	if (self->dropdowns != NULL)
		g_ptr_array_unref(self->dropdowns);

	g_clear_pointer(&self->module_configs, g_hash_table_unref);
	g_clear_pointer(&self->monitor_configs, g_hash_table_unref);
	g_list_free_full(self->profiles, output_profile_free);
	self->profiles = NULL;

	G_OBJECT_CLASS(gowl_config_parent_class)->finalize(object);
}

/* --- Class init --- */

/**
 * gowl_config_class_init:
 * @klass: the #GowlConfigClass
 *
 * Installs GObject properties and signals on the #GowlConfig class.
 */
static void
gowl_config_class_init(GowlConfigClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);

	object_class->set_property = gowl_config_set_property;
	object_class->get_property = gowl_config_get_property;
	object_class->finalize     = gowl_config_finalize;

	/* --- Install properties --- */

	properties[GOWL_CONFIG_PROP_BORDER_WIDTH] =
		g_param_spec_int("border-width",
		                  "Border Width",
		                  "Window border width in pixels",
		                  0, 100,
		                  GOWL_CONFIG_DEFAULT_BORDER_WIDTH,
		                  G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

	properties[GOWL_CONFIG_PROP_BORDER_COLOR_FOCUS] =
		g_param_spec_string("border-color-focus",
		                     "Border Color Focus",
		                     "Hex colour for focused window border",
		                     GOWL_CONFIG_DEFAULT_BORDER_COLOR_FOCUS,
		                     G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

	properties[GOWL_CONFIG_PROP_BORDER_COLOR_UNFOCUS] =
		g_param_spec_string("border-color-unfocus",
		                     "Border Color Unfocus",
		                     "Hex colour for unfocused window border",
		                     GOWL_CONFIG_DEFAULT_BORDER_COLOR_UNFOCUS,
		                     G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

	properties[GOWL_CONFIG_PROP_BORDER_COLOR_URGENT] =
		g_param_spec_string("border-color-urgent",
		                     "Border Color Urgent",
		                     "Hex colour for urgent window border",
		                     GOWL_CONFIG_DEFAULT_BORDER_COLOR_URGENT,
		                     G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

	properties[GOWL_CONFIG_PROP_MFACT] =
		g_param_spec_double("mfact",
		                     "Master Factor",
		                     "Fraction of screen width for the master area",
		                     0.05, 0.95,
		                     GOWL_CONFIG_DEFAULT_MFACT,
		                     G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

	properties[GOWL_CONFIG_PROP_NMASTER] =
		g_param_spec_int("nmaster",
		                  "Number of Masters",
		                  "Number of windows in the master area",
		                  0, 100,
		                  GOWL_CONFIG_DEFAULT_NMASTER,
		                  G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

	properties[GOWL_CONFIG_PROP_TAG_COUNT] =
		g_param_spec_int("tag-count",
		                  "Tag Count",
		                  "Number of tag (workspace) slots",
		                  1, GOWL_MAX_TAGS,
		                  GOWL_CONFIG_DEFAULT_TAG_COUNT,
		                  G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

	properties[GOWL_CONFIG_PROP_REPEAT_RATE] =
		g_param_spec_int("repeat-rate",
		                  "Repeat Rate",
		                  "Keyboard repeat rate in keys per second",
		                  1, 1000,
		                  GOWL_CONFIG_DEFAULT_REPEAT_RATE,
		                  G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

	properties[GOWL_CONFIG_PROP_REPEAT_DELAY] =
		g_param_spec_int("repeat-delay",
		                  "Repeat Delay",
		                  "Keyboard repeat delay in milliseconds",
		                  1, 10000,
		                  GOWL_CONFIG_DEFAULT_REPEAT_DELAY,
		                  G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

	properties[GOWL_CONFIG_PROP_TERMINAL] =
		g_param_spec_string("terminal",
		                     "Terminal",
		                     "Default terminal emulator command",
		                     GOWL_CONFIG_DEFAULT_TERMINAL,
		                     G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

	properties[GOWL_CONFIG_PROP_MENU] =
		g_param_spec_string("menu",
		                     "Menu",
		                     "Application launcher / menu command",
		                     GOWL_CONFIG_DEFAULT_MENU,
		                     G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

	properties[GOWL_CONFIG_PROP_SLOPPYFOCUS] =
		g_param_spec_boolean("sloppyfocus",
		                      "Sloppy Focus",
		                      "Whether focus follows the mouse pointer",
		                      GOWL_CONFIG_DEFAULT_SLOPPYFOCUS,
		                      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

	properties[GOWL_CONFIG_PROP_MANAGE_LID] =
		g_param_spec_boolean("manage-lid",
		                      "Manage Lid",
		                      "Power off internal panels when the laptop "
		                      "lid is shut and an external display is present",
		                      GOWL_CONFIG_DEFAULT_MANAGE_LID,
		                      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

	properties[GOWL_CONFIG_PROP_XKB_LAYOUT] =
		g_param_spec_string("xkb-layout", "XKB Layout",
		                    "Keyboard layout list, e.g. \"us,de\"; unset "
		                    "takes XKB_DEFAULT_LAYOUT / the system default",
		                    NULL, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
	properties[GOWL_CONFIG_PROP_XKB_VARIANT] =
		g_param_spec_string("xkb-variant", "XKB Variant",
		                    "Keyboard variant list, one per layout",
		                    NULL, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
	properties[GOWL_CONFIG_PROP_XKB_MODEL] =
		g_param_spec_string("xkb-model", "XKB Model", "Keyboard model",
		                    NULL, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
	properties[GOWL_CONFIG_PROP_XKB_OPTIONS] =
		g_param_spec_string("xkb-options", "XKB Options",
		                    "Keyboard options, e.g. "
		                    "\"grp:alt_shift_toggle,caps:escape\"",
		                    NULL, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
	properties[GOWL_CONFIG_PROP_XKB_RULES] =
		g_param_spec_string("xkb-rules", "XKB Rules", "Keyboard rules set",
		                    NULL, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
	properties[GOWL_CONFIG_PROP_XKB_FILE] =
		g_param_spec_string("xkb-file", "XKB File",
		                    "A complete keymap file, overriding the "
		                    "rules/model/layout/variant/options set",
		                    NULL, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

	properties[GOWL_CONFIG_PROP_IDLE_TIMEOUT] =
		g_param_spec_int("idle-timeout",
		                 "Idle Timeout",
		                 "Seconds without input before the session is "
		                 "idle; 0 never",
		                 0, G_MAXINT, GOWL_CONFIG_DEFAULT_IDLE_TIMEOUT,
		                 G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

	properties[GOWL_CONFIG_PROP_DPMS_TIMEOUT] =
		g_param_spec_int("dpms-timeout",
		                 "DPMS Timeout",
		                 "Seconds without input before every output is "
		                 "powered off; 0 never",
		                 0, G_MAXINT, GOWL_CONFIG_DEFAULT_DPMS_TIMEOUT,
		                 G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

	properties[GOWL_CONFIG_PROP_ALLOW_TEARING] =
		g_param_spec_boolean("allow-tearing",
		                      "Allow Tearing",
		                      "Present a fullscreen window that asks for "
		                      "tearing without waiting for vblank",
		                      GOWL_CONFIG_DEFAULT_ALLOW_TEARING,
		                      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

	/*
	 * The HDR five, as PROPERTIES rather than plain fields.
	 *
	 * An embedder that owns its own configuration can only reach a
	 * setting through the property system: cmacs deliberately never
	 * reads ~/.config/gowl/config.yaml, so a key that exists only in
	 * the YAML parser exists for standalone gowl and for nobody else.
	 * These were exactly that until somebody went looking for the
	 * switch and found the file it was documented in is never opened.
	 */
	properties[GOWL_CONFIG_PROP_HDR_ENCODE] =
		g_param_spec_boolean("hdr-encode",
		                     "HDR Encode",
		                     "Encode the desktop for PQ where the "
		                     "renderer cannot convert colour",
		                     GOWL_CONFIG_DEFAULT_HDR_ENCODE,
		                     G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

	properties[GOWL_CONFIG_PROP_HDR_BPC] =
		g_param_spec_int("hdr-bpc",
		                 "HDR Bits Per Channel",
		                 "0 asks for ten bits and settles for eight; "
		                 "8 never asks",
		                 0, 10, GOWL_CONFIG_DEFAULT_HDR_BPC,
		                 G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

	properties[GOWL_CONFIG_PROP_HDR_SDR_WHITE] =
		g_param_spec_double("hdr-sdr-white",
		                    "HDR SDR White",
		                    "Where SDR white lands in the HDR signal, "
		                    "in cd/m2",
		                    40.0, 600.0, GOWL_CONFIG_DEFAULT_HDR_SDR_WHITE,
		                    G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

	properties[GOWL_CONFIG_PROP_HDR_UNMANAGED] =
		g_param_spec_boolean("hdr-unmanaged",
		                     "HDR Unmanaged",
		                     "Allow HDR where the compositor cannot "
		                     "convert colour for it",
		                     GOWL_CONFIG_DEFAULT_HDR_UNMANAGED,
		                     G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

	properties[GOWL_CONFIG_PROP_HDR_ADVERTISE_PQ] =
		g_param_spec_boolean("hdr-advertise-pq",
		                     "HDR Advertise PQ",
		                     "Tell clients PQ and BT.2020 content is "
		                     "accepted, though nothing converts it",
		                     GOWL_CONFIG_DEFAULT_HDR_ADVERTISE_PQ,
		                     G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

	properties[GOWL_CONFIG_PROP_FOCUS_ON_ACTIVATE] =
		g_param_spec_string("focus-on-activate",
		                    "Focus On Activate",
		                    "What a window's activation request does: "
		                    "smart, urgent, focus or none",
		                    GOWL_CONFIG_DEFAULT_FOCUS_ON_ACTIVATE,
		                    G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

	properties[GOWL_CONFIG_PROP_INPUT_RECORDING] =
		g_param_spec_boolean("input-recording",
		                      "Input Recording",
		                      "Allow a recording of real key and pointer "
		                      "input to be started.  Separate from -- and "
		                      "never implied by -- the tools that inject "
		                      "input, because capturing what somebody "
		                      "types is a different permission from "
		                      "clicking on their behalf.",
		                      GOWL_CONFIG_DEFAULT_INPUT_RECORDING,
		                      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

	/*
	 * A PROPERTY and not only a YAML key, because cmacs deliberately
	 * never opens ~/.config/gowl/config.yaml -- a setting that exists
	 * only in the parser exists for standalone gowl and for nobody
	 * else.
	 */
	properties[GOWL_CONFIG_PROP_NO_FX_APPS] =
		g_param_spec_string("no-fx-apps",
		                     "No-FX Apps",
		                     "Comma-separated app_ids and process names "
		                     "that get no backdrop, shadow or animation. "
		                     "Matched against the window's app_id and "
		                     "against the command name of its process "
		                     "and every ancestor.  Added to the built-in "
		                     "list (steam, steamwebhelper, "
		                     "mutter-devkit), never replacing it.",
		                     GOWL_CONFIG_DEFAULT_NO_FX_APPS,
		                     G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

	properties[GOWL_CONFIG_PROP_INPUT_RECORDING_DENY_APPS] =
		g_param_spec_string("input-recording-deny-apps",
		                     "Input Recording Deny Apps",
		                     "Comma-separated glob patterns.  Input is "
		                     "not recorded while the focused window's "
		                     "app-id or title matches one.  Added to the "
		                     "built-in list of credential prompts, never "
		                     "replacing it.",
		                     GOWL_CONFIG_DEFAULT_INPUT_RECORDING_DENY_APPS,
		                     G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

	properties[GOWL_CONFIG_PROP_LOG_LEVEL] =
		g_param_spec_string("log-level",
		                     "Log Level",
		                     "Logging verbosity (debug, info, warning, error)",
		                     GOWL_CONFIG_DEFAULT_LOG_LEVEL,
		                     G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

	properties[GOWL_CONFIG_PROP_LOG_FILE] =
		g_param_spec_string("log-file",
		                     "Log File",
		                     "Path to log file (\"stderr\" for stderr only)",
		                     GOWL_CONFIG_DEFAULT_LOG_FILE,
		                     G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

	properties[GOWL_CONFIG_PROP_EVALUATE_GOWL_CONFIG_WITH_CMACS] =
		g_param_spec_boolean("evaluate-gowl-config-with-cmacs",
		                      "Evaluate Gowl Config With Cmacs",
		                      "When FALSE, cmacs `--gowl` resets all "
		                      "other config values to defaults after "
		                      "parsing.  Ignored by standalone gowl.",
		                      GOWL_CONFIG_DEFAULT_EVALUATE_GOWL_CONFIG_WITH_CMACS,
		                      G_PARAM_READWRITE
		                      | G_PARAM_EXPLICIT_NOTIFY
		                      | G_PARAM_STATIC_STRINGS);

	properties[GOWL_CONFIG_PROP_EVALUATE_C_CONFIG_WITH_CMACS] =
		g_param_spec_boolean("evaluate-c-config-with-cmacs",
		                      "Evaluate C Config With Cmacs",
		                      "When FALSE, cmacs `--gowl` skips loading "
		                      "the user's C config entirely.  Ignored "
		                      "by standalone gowl.",
		                      GOWL_CONFIG_DEFAULT_EVALUATE_C_CONFIG_WITH_CMACS,
		                      G_PARAM_READWRITE
		                      | G_PARAM_EXPLICIT_NOTIFY
		                      | G_PARAM_STATIC_STRINGS);

	g_object_class_install_properties(object_class,
	                                  GOWL_CONFIG_PROP_LAST,
	                                  properties);

	/* --- Install signals --- */

	/**
	 * GowlConfig::changed:
	 * @self: the #GowlConfig that changed
	 * @property_name: the name of the property that changed
	 *
	 * Emitted whenever a configuration property is modified.
	 */
	signals[SIGNAL_CHANGED] =
		g_signal_new("changed",
		             G_TYPE_FROM_CLASS(klass),
		             G_SIGNAL_RUN_LAST,
		             0,
		             NULL, NULL,
		             NULL,
		             G_TYPE_NONE, 1,
		             G_TYPE_STRING);

	/**
	 * GowlConfig::reloaded:
	 * @self: the #GowlConfig that was reloaded
	 *
	 * Emitted after a full configuration reload completes successfully.
	 */
	signals[SIGNAL_RELOADED] =
		g_signal_new("reloaded",
		             G_TYPE_FROM_CLASS(klass),
		             G_SIGNAL_RUN_LAST,
		             0,
		             NULL, NULL,
		             NULL,
		             G_TYPE_NONE, 0);
}

/* --- Instance init --- */

/**
 * gowl_config_init:
 * @self: the #GowlConfig instance being initialised
 *
 * Sets all fields to their default values and allocates the
 * keybind and rule arrays.
 */
static void
gowl_config_init(GowlConfig *self)
{
	self->border_width        = GOWL_CONFIG_DEFAULT_BORDER_WIDTH;
	self->border_color_focus  = g_strdup(GOWL_CONFIG_DEFAULT_BORDER_COLOR_FOCUS);
	self->border_color_unfocus = g_strdup(GOWL_CONFIG_DEFAULT_BORDER_COLOR_UNFOCUS);
	self->border_color_urgent = g_strdup(GOWL_CONFIG_DEFAULT_BORDER_COLOR_URGENT);
	self->palette_name        = g_strdup(GOWL_CONFIG_DEFAULT_PALETTE);
	self->palette             = gowl_palette_new_builtin(self->palette_name);
	self->palette_override    = gowl_palette_new();
	gowl_config_reresolve_colors(self);
	self->mfact               = GOWL_CONFIG_DEFAULT_MFACT;
	self->scroll_column_width = GOWL_CONFIG_DEFAULT_SCROLL_COLUMN_WIDTH;
	self->animations          = TRUE;
	self->animation_duration  = GOWL_CONFIG_DEFAULT_ANIMATION_DURATION;
	self->animation_duration_open =
		GOWL_CONFIG_DEFAULT_ANIMATION_DURATION_OPEN;
	self->animation_duration_close =
		GOWL_CONFIG_DEFAULT_ANIMATION_DURATION_CLOSE;
	self->animation_curve     = g_strdup(GOWL_CONFIG_DEFAULT_ANIMATION_CURVE);
	self->animation_curve_open = g_strdup(GOWL_CONFIG_DEFAULT_ANIMATION_CURVE_OPEN);
	self->animation_curve_close = g_strdup(GOWL_CONFIG_DEFAULT_ANIMATION_CURVE_CLOSE);
	self->animation_popin_scale = GOWL_CONFIG_DEFAULT_ANIMATION_POPIN_SCALE;
	self->animation_jiggle_strength = 1.0;

	self->cube                = TRUE;
	self->cube_duration       = GOWL_CONFIG_DEFAULT_CUBE_DURATION;
	self->cube_step_duration  = GOWL_CONFIG_DEFAULT_CUBE_STEP_DURATION;
	self->cube_curve          = g_strdup(GOWL_CONFIG_DEFAULT_CUBE_CURVE);
	self->cube_faces          = GOWL_CONFIG_DEFAULT_CUBE_FACES;
	self->cube_zoom           = GOWL_CONFIG_DEFAULT_CUBE_ZOOM;
	self->cube_pitch          = GOWL_CONFIG_DEFAULT_CUBE_PITCH;
	self->cube_shading        = GOWL_CONFIG_DEFAULT_CUBE_SHADING;
	self->cube_reflection     = GOWL_CONFIG_DEFAULT_CUBE_REFLECTION;
	self->cube_motion_blur    = GOWL_CONFIG_DEFAULT_CUBE_MOTION_BLUR;
	self->cube_backdrop_color =
		g_strdup(GOWL_CONFIG_DEFAULT_CUBE_BACKDROP_COLOR);
	self->cube_caps           = TRUE;
	self->cube_all_monitors   = FALSE;
	self->cube_gesture        = TRUE;

	self->magnifier               = TRUE;
	self->magnifier_max           = GOWL_CONFIG_DEFAULT_MAGNIFIER_MAX;
	self->magnifier_step          = GOWL_CONFIG_DEFAULT_MAGNIFIER_STEP;
	self->magnifier_smoothing     = GOWL_CONFIG_DEFAULT_MAGNIFIER_SMOOTHING;
	self->magnifier_follow_cursor = TRUE;
	self->magnifier_smooth        = TRUE;
	self->magnifier_modifier      =
		g_strdup(GOWL_CONFIG_DEFAULT_MAGNIFIER_MODIFIER);

	self->expo                = TRUE;
	self->expo_duration       = GOWL_CONFIG_DEFAULT_EXPO_DURATION;
	self->expo_curve          = g_strdup(GOWL_CONFIG_DEFAULT_EXPO_CURVE);
	self->expo_tags           = GOWL_CONFIG_DEFAULT_EXPO_TAGS;
	self->expo_columns        = 0;
	self->expo_gap            = GOWL_CONFIG_DEFAULT_EXPO_GAP;
	self->expo_corner         = GOWL_CONFIG_DEFAULT_EXPO_CORNER;
	self->expo_dim            = GOWL_CONFIG_DEFAULT_EXPO_DIM;
	self->expo_hide_empty     = FALSE;
	self->expo_backdrop_color =
		g_strdup(GOWL_CONFIG_DEFAULT_EXPO_BACKDROP_COLOR);

	self->switcher                = TRUE;
	self->switcher_duration       = GOWL_CONFIG_DEFAULT_SWITCHER_DURATION;
	self->switcher_curve          = g_strdup(GOWL_CONFIG_DEFAULT_SWITCHER_CURVE);
	self->switcher_scale          = GOWL_CONFIG_DEFAULT_SWITCHER_SCALE;
	self->switcher_spacing        = GOWL_CONFIG_DEFAULT_SWITCHER_SPACING;
	self->switcher_angle          = GOWL_CONFIG_DEFAULT_SWITCHER_ANGLE;
	self->switcher_reflection     = GOWL_CONFIG_DEFAULT_SWITCHER_REFLECTION;
	self->switcher_all_tags       = FALSE;
	self->switcher_backdrop_color =
		g_strdup(GOWL_CONFIG_DEFAULT_SWITCHER_BACKDROP_COLOR);

	self->blur             = TRUE;
	self->blur_downscale   = GOWL_CONFIG_DEFAULT_BLUR_DOWNSCALE;
	self->blur_passes      = GOWL_CONFIG_DEFAULT_BLUR_PASSES;
	self->blur_brightness  = GOWL_CONFIG_DEFAULT_BLUR_BRIGHTNESS;
	self->shadow           = TRUE;
	self->shadow_radius    = GOWL_CONFIG_DEFAULT_SHADOW_RADIUS;
	self->shadow_opacity   = GOWL_CONFIG_DEFAULT_SHADOW_OPACITY;
	self->shadow_offset_x  = GOWL_CONFIG_DEFAULT_SHADOW_OFFSET_X;
	self->shadow_offset_y  = GOWL_CONFIG_DEFAULT_SHADOW_OFFSET_Y;
	self->shadow_color     = g_strdup(GOWL_CONFIG_DEFAULT_SHADOW_COLOR);
	/* Glass rather than blur out of the box.  Both modules read this and
	 * only one of them draws; see GowlBackdropStyle. */
	self->backdrop_style     = GOWL_BACKDROP_GLASS;
	self->glass_bevel        = GOWL_CONFIG_DEFAULT_GLASS_BEVEL;
	self->glass_thickness    = GOWL_CONFIG_DEFAULT_GLASS_THICKNESS;
	self->glass_slope        = GOWL_CONFIG_DEFAULT_GLASS_SLOPE;
	self->glass_shape        = g_strdup(GOWL_CONFIG_DEFAULT_GLASS_SHAPE);
	self->glass_dispersion   = GOWL_CONFIG_DEFAULT_GLASS_DISPERSION;
	self->glass_rim          = GOWL_CONFIG_DEFAULT_GLASS_RIM;
	self->glass_shade        = GOWL_CONFIG_DEFAULT_GLASS_SHADE;
	self->glass_edge_width   = GOWL_CONFIG_DEFAULT_GLASS_EDGE_WIDTH;
	self->glass_saturation   = GOWL_CONFIG_DEFAULT_GLASS_SATURATION;
	self->glass_clarity      = GOWL_CONFIG_DEFAULT_GLASS_CLARITY;
	self->glass_centre_clarity = GOWL_CONFIG_DEFAULT_GLASS_CENTRE_CLARITY;
	self->glass_lens         = GOWL_CONFIG_DEFAULT_GLASS_LENS;
	self->glass_sheen        = GOWL_CONFIG_DEFAULT_GLASS_SHEEN;
	self->glass_light        = GOWL_CONFIG_DEFAULT_GLASS_LIGHT;
	self->glass_tint         = g_strdup(GOWL_CONFIG_DEFAULT_GLASS_TINT);
	self->glass_brightness   = GOWL_CONFIG_DEFAULT_GLASS_BRIGHTNESS;
	self->glass_opacity      = GOWL_CONFIG_DEFAULT_GLASS_OPACITY;
	self->glass_frost        = GOWL_CONFIG_DEFAULT_GLASS_FROST;
	self->glass_frost_passes = GOWL_CONFIG_DEFAULT_GLASS_FROST_PASSES;
	self->water_preset       = g_strdup(GOWL_CONFIG_DEFAULT_WATER_PRESET);
	self->water_intensity    = GOWL_CONFIG_DEFAULT_WATER_INTENSITY;
	self->water_fps          = GOWL_CONFIG_DEFAULT_WATER_FPS;
	self->water_scale        = GOWL_CONFIG_DEFAULT_WATER_SCALE;
	self->water_tint         = g_strdup(GOWL_CONFIG_DEFAULT_WATER_TINT);
	self->water_clarity      = GOWL_CONFIG_DEFAULT_WATER_CLARITY;
	self->water_opacity      = GOWL_CONFIG_DEFAULT_WATER_OPACITY;
	self->water_brightness   = GOWL_CONFIG_DEFAULT_WATER_BRIGHTNESS;
	self->water_light        = GOWL_CONFIG_DEFAULT_WATER_LIGHT;
	self->water_frost        = GOWL_CONFIG_DEFAULT_WATER_FROST;
	self->water_frost_passes = GOWL_CONFIG_DEFAULT_WATER_FROST_PASSES;
	self->water_amplitude    = GOWL_CONFIG_WATER_FROM_PRESET;
	self->water_wavelength   = GOWL_CONFIG_WATER_FROM_PRESET;
	self->water_choppiness   = GOWL_CONFIG_WATER_FROM_PRESET;
	self->water_depth        = GOWL_CONFIG_WATER_FROM_PRESET;
	self->water_drops        = GOWL_CONFIG_WATER_FROM_PRESET;
	self->water_shore        = GOWL_CONFIG_WATER_FROM_PRESET;
	self->water_specular     = GOWL_CONFIG_WATER_FROM_PRESET;
	self->water_caustics     = GOWL_CONFIG_WATER_FROM_PRESET;
	self->water_foam         = GOWL_CONFIG_WATER_FROM_PRESET;
	self->water_fresnel      = GOWL_CONFIG_WATER_FROM_PRESET;
	self->water_speed        = GOWL_CONFIG_WATER_FROM_PRESET;
	self->rain_preset        = g_strdup(GOWL_CONFIG_DEFAULT_RAIN_PRESET);
	self->rain_intensity     = GOWL_CONFIG_DEFAULT_RAIN_INTENSITY;
	self->soap_preset = g_strdup(GOWL_CONFIG_DEFAULT_SOAP_PRESET);
	self->soap_intensity = GOWL_CONFIG_DEFAULT_SOAP_INTENSITY;
	self->soap_fps = GOWL_CONFIG_DEFAULT_SOAP_FPS;
	self->soap_scale = GOWL_CONFIG_DEFAULT_SOAP_SCALE;
	self->soap_tint = g_strdup(GOWL_CONFIG_DEFAULT_SOAP_TINT);
	self->soap_clarity = GOWL_CONFIG_DEFAULT_SOAP_CLARITY;
	self->soap_opacity = GOWL_CONFIG_DEFAULT_SOAP_OPACITY;
	self->soap_brightness = GOWL_CONFIG_DEFAULT_SOAP_BRIGHTNESS;
	self->soap_light = GOWL_CONFIG_DEFAULT_SOAP_LIGHT;
	self->soap_frost = GOWL_CONFIG_DEFAULT_SOAP_FROST;
	self->soap_frost_passes = GOWL_CONFIG_DEFAULT_SOAP_FROST_PASSES;
	self->soap_thickness = GOWL_CONFIG_SOAP_FROM_PRESET;
	self->soap_thin = GOWL_CONFIG_SOAP_FROM_PRESET;
	self->soap_drain = GOWL_CONFIG_SOAP_FROM_PRESET;
	self->soap_turbulence = GOWL_CONFIG_SOAP_FROM_PRESET;
	self->soap_swirl = GOWL_CONFIG_SOAP_FROM_PRESET;
	self->soap_index = GOWL_CONFIG_SOAP_FROM_PRESET;
	self->soap_gain = GOWL_CONFIG_SOAP_FROM_PRESET;
	self->soap_sheen = GOWL_CONFIG_SOAP_FROM_PRESET;
	self->soap_wedge = GOWL_CONFIG_SOAP_FROM_PRESET;
	self->soap_pop = GOWL_CONFIG_SOAP_FROM_PRESET;
	self->soap_meniscus = GOWL_CONFIG_SOAP_FROM_PRESET;
	self->soap_fog = GOWL_CONFIG_SOAP_FROM_PRESET;
	self->soap_speed = GOWL_CONFIG_SOAP_FROM_PRESET;
	self->soap_life = GOWL_CONFIG_SOAP_FROM_PRESET;

	self->embers_preset = g_strdup(GOWL_CONFIG_DEFAULT_EMBERS_PRESET);
	self->embers_intensity = GOWL_CONFIG_DEFAULT_EMBERS_INTENSITY;
	self->embers_fps = GOWL_CONFIG_DEFAULT_EMBERS_FPS;
	self->embers_scale = GOWL_CONFIG_DEFAULT_EMBERS_SCALE;
	self->embers_tint = g_strdup(GOWL_CONFIG_DEFAULT_EMBERS_TINT);
	self->embers_clarity = GOWL_CONFIG_DEFAULT_EMBERS_CLARITY;
	self->embers_opacity = GOWL_CONFIG_DEFAULT_EMBERS_OPACITY;
	self->embers_brightness = GOWL_CONFIG_DEFAULT_EMBERS_BRIGHTNESS;
	self->embers_frost = GOWL_CONFIG_DEFAULT_EMBERS_FROST;
	self->embers_frost_passes = GOWL_CONFIG_DEFAULT_EMBERS_FROST_PASSES;
	self->embers_column = GOWL_CONFIG_EMBERS_FROM_PRESET;
	self->embers_density = GOWL_CONFIG_EMBERS_FROM_PRESET;
	self->embers_ember = GOWL_CONFIG_EMBERS_FROM_PRESET;
	self->embers_spacing = GOWL_CONFIG_EMBERS_FROM_PRESET;
	self->embers_sway = GOWL_CONFIG_EMBERS_FROM_PRESET;
	self->embers_drag = GOWL_CONFIG_EMBERS_FROM_PRESET;
	self->embers_temperature = GOWL_CONFIG_EMBERS_FROM_PRESET;
	self->embers_cool = GOWL_CONFIG_EMBERS_FROM_PRESET;
	self->embers_flicker = GOWL_CONFIG_EMBERS_FROM_PRESET;
	self->embers_ash = GOWL_CONFIG_EMBERS_FROM_PRESET;
	self->embers_glow = GOWL_CONFIG_EMBERS_FROM_PRESET;
	self->embers_hearth = GOWL_CONFIG_EMBERS_FROM_PRESET;
	self->embers_haze = GOWL_CONFIG_EMBERS_FROM_PRESET;
	self->embers_haze_scale = GOWL_CONFIG_EMBERS_FROM_PRESET;
	self->embers_fog = GOWL_CONFIG_EMBERS_FROM_PRESET;
	self->embers_speed = GOWL_CONFIG_EMBERS_FROM_PRESET;

	self->submerged_preset = g_strdup(GOWL_CONFIG_DEFAULT_SUBMERGED_PRESET);
	self->submerged_intensity = GOWL_CONFIG_DEFAULT_SUBMERGED_INTENSITY;
	self->submerged_fps = GOWL_CONFIG_DEFAULT_SUBMERGED_FPS;
	self->submerged_scale = GOWL_CONFIG_DEFAULT_SUBMERGED_SCALE;
	self->submerged_water = g_strdup(GOWL_CONFIG_DEFAULT_SUBMERGED_WATER);
	self->submerged_clarity = GOWL_CONFIG_DEFAULT_SUBMERGED_CLARITY;
	self->submerged_opacity = GOWL_CONFIG_DEFAULT_SUBMERGED_OPACITY;
	self->submerged_brightness = GOWL_CONFIG_DEFAULT_SUBMERGED_BRIGHTNESS;
	self->submerged_frost = GOWL_CONFIG_DEFAULT_SUBMERGED_FROST;
	self->submerged_frost_passes = GOWL_CONFIG_DEFAULT_SUBMERGED_FROST_PASSES;
	self->submerged_depth = GOWL_CONFIG_SUBMERGED_FROM_PRESET;
	self->submerged_extinction_r = GOWL_CONFIG_SUBMERGED_FROM_PRESET;
	self->submerged_extinction_g = GOWL_CONFIG_SUBMERGED_FROM_PRESET;
	self->submerged_extinction_b = GOWL_CONFIG_SUBMERGED_FROM_PRESET;
	self->submerged_murk = GOWL_CONFIG_SUBMERGED_FROM_PRESET;
	self->submerged_caustics = GOWL_CONFIG_SUBMERGED_FROM_PRESET;
	self->submerged_caustic_scale = GOWL_CONFIG_SUBMERGED_FROM_PRESET;
	self->submerged_shafts = GOWL_CONFIG_SUBMERGED_FROM_PRESET;
	self->submerged_shaft_lean = GOWL_CONFIG_SUBMERGED_FROM_PRESET;
	self->submerged_motes = GOWL_CONFIG_SUBMERGED_FROM_PRESET;
	self->submerged_mote_size = GOWL_CONFIG_SUBMERGED_FROM_PRESET;
	self->submerged_surface = GOWL_CONFIG_SUBMERGED_FROM_PRESET;
	self->submerged_sway = GOWL_CONFIG_SUBMERGED_FROM_PRESET;
	self->submerged_fog = GOWL_CONFIG_SUBMERGED_FROM_PRESET;
	self->submerged_speed = GOWL_CONFIG_SUBMERGED_FROM_PRESET;
	self->submerged_drift = GOWL_CONFIG_SUBMERGED_FROM_PRESET;

	self->dew_preset = g_strdup(GOWL_CONFIG_DEFAULT_DEW_PRESET);
	self->dew_intensity = GOWL_CONFIG_DEFAULT_DEW_INTENSITY;
	self->dew_fps = GOWL_CONFIG_DEFAULT_DEW_FPS;
	self->dew_scale = GOWL_CONFIG_DEFAULT_DEW_SCALE;
	self->dew_tint = g_strdup(GOWL_CONFIG_DEFAULT_DEW_TINT);
	self->dew_clarity = GOWL_CONFIG_DEFAULT_DEW_CLARITY;
	self->dew_opacity = GOWL_CONFIG_DEFAULT_DEW_OPACITY;
	self->dew_brightness = GOWL_CONFIG_DEFAULT_DEW_BRIGHTNESS;
	self->dew_light = GOWL_CONFIG_DEFAULT_DEW_LIGHT;
	self->dew_frost = GOWL_CONFIG_DEFAULT_DEW_FROST;
	self->dew_frost_passes = GOWL_CONFIG_DEFAULT_DEW_FROST_PASSES;
	self->dew_radials = GOWL_CONFIG_DEW_FROM_PRESET;
	self->dew_pitch = GOWL_CONFIG_DEW_FROM_PRESET;
	self->dew_thread = GOWL_CONFIG_DEW_FROM_PRESET;
	self->dew_drop = GOWL_CONFIG_DEW_FROM_PRESET;
	self->dew_spacing = GOWL_CONFIG_DEW_FROM_PRESET;
	self->dew_sag = GOWL_CONFIG_DEW_FROM_PRESET;
	self->dew_depth = GOWL_CONFIG_DEW_FROM_PRESET;
	self->dew_bulge = GOWL_CONFIG_DEW_FROM_PRESET;
	self->dew_silk = GOWL_CONFIG_DEW_FROM_PRESET;
	self->dew_glint = GOWL_CONFIG_DEW_FROM_PRESET;
	self->dew_shine = GOWL_CONFIG_DEW_FROM_PRESET;
	self->dew_rim = GOWL_CONFIG_DEW_FROM_PRESET;
	self->dew_sway = GOWL_CONFIG_DEW_FROM_PRESET;
	self->dew_fog = GOWL_CONFIG_DEW_FROM_PRESET;
	self->dew_speed = GOWL_CONFIG_DEW_FROM_PRESET;


	self->bokeh_radius       = GOWL_CONFIG_DEFAULT_BOKEH_RADIUS;
	self->bokeh_downscale    = GOWL_CONFIG_DEFAULT_BOKEH_DOWNSCALE;
	self->bokeh_samples      = GOWL_CONFIG_DEFAULT_BOKEH_SAMPLES;
	self->bokeh_blades       = GOWL_CONFIG_DEFAULT_BOKEH_BLADES;
	self->bokeh_rotation     = GOWL_CONFIG_DEFAULT_BOKEH_ROTATION;
	self->bokeh_highlight    = GOWL_CONFIG_DEFAULT_BOKEH_HIGHLIGHT;
	self->bokeh_threshold    = GOWL_CONFIG_DEFAULT_BOKEH_THRESHOLD;
	self->bokeh_edge         = GOWL_CONFIG_DEFAULT_BOKEH_EDGE;
	self->bokeh_brightness   = GOWL_CONFIG_DEFAULT_BOKEH_BRIGHTNESS;

	self->rain_lightning     = GOWL_CONFIG_DEFAULT_RAIN_LIGHTNING;
	self->rain_lightning_rate = GOWL_CONFIG_DEFAULT_RAIN_LIGHTNING_RATE;
	self->rain_lightning_power = GOWL_CONFIG_DEFAULT_RAIN_LIGHTNING_POWER;
	self->rain_fps           = GOWL_CONFIG_DEFAULT_RAIN_FPS;
	self->rain_scale         = GOWL_CONFIG_DEFAULT_RAIN_SCALE;
	self->rain_tint          = g_strdup(GOWL_CONFIG_DEFAULT_RAIN_TINT);
	self->rain_clarity       = GOWL_CONFIG_DEFAULT_RAIN_CLARITY;
	self->rain_opacity       = GOWL_CONFIG_DEFAULT_RAIN_OPACITY;
	self->rain_brightness    = GOWL_CONFIG_DEFAULT_RAIN_BRIGHTNESS;
	self->rain_light         = GOWL_CONFIG_DEFAULT_RAIN_LIGHT;
	self->rain_frost         = GOWL_CONFIG_DEFAULT_RAIN_FROST;
	self->rain_frost_passes  = GOWL_CONFIG_DEFAULT_RAIN_FROST_PASSES;
	self->rain_cell          = GOWL_CONFIG_RAIN_FROM_PRESET;
	self->rain_density       = GOWL_CONFIG_RAIN_FROM_PRESET;
	self->rain_bulge         = GOWL_CONFIG_RAIN_FROM_PRESET;
	self->rain_depth         = GOWL_CONFIG_RAIN_FROM_PRESET;
	self->rain_runs          = GOWL_CONFIG_RAIN_FROM_PRESET;
	self->rain_run_width     = GOWL_CONFIG_RAIN_FROM_PRESET;
	self->rain_run_length    = GOWL_CONFIG_RAIN_FROM_PRESET;
	self->rain_beads         = GOWL_CONFIG_RAIN_FROM_PRESET;
	self->rain_fog           = GOWL_CONFIG_RAIN_FROM_PRESET;
	self->rain_specular      = GOWL_CONFIG_RAIN_FROM_PRESET;
	self->rain_impact        = GOWL_CONFIG_RAIN_FROM_PRESET;
	self->rain_speed         = GOWL_CONFIG_RAIN_FROM_PRESET;

	self->fizz_preset        = g_strdup(GOWL_CONFIG_DEFAULT_FIZZ_PRESET);
	self->fizz_intensity     = GOWL_CONFIG_DEFAULT_FIZZ_INTENSITY;
	self->fizz_fps           = GOWL_CONFIG_DEFAULT_FIZZ_FPS;
	self->fizz_scale         = GOWL_CONFIG_DEFAULT_FIZZ_SCALE;
	self->fizz_tint          = g_strdup(GOWL_CONFIG_DEFAULT_FIZZ_TINT);
	self->fizz_clarity       = GOWL_CONFIG_DEFAULT_FIZZ_CLARITY;
	self->fizz_opacity       = GOWL_CONFIG_DEFAULT_FIZZ_OPACITY;
	self->fizz_brightness    = GOWL_CONFIG_DEFAULT_FIZZ_BRIGHTNESS;
	self->fizz_light         = GOWL_CONFIG_DEFAULT_FIZZ_LIGHT;
	self->fizz_frost         = GOWL_CONFIG_DEFAULT_FIZZ_FROST;
	self->fizz_frost_passes  = GOWL_CONFIG_DEFAULT_FIZZ_FROST_PASSES;
	self->fizz_cell          = GOWL_CONFIG_FIZZ_FROM_PRESET;
	self->fizz_bubble        = GOWL_CONFIG_FIZZ_FROM_PRESET;
	self->fizz_growth        = GOWL_CONFIG_FIZZ_FROM_PRESET;
	self->fizz_sites         = GOWL_CONFIG_FIZZ_FROM_PRESET;
	self->fizz_site_width    = GOWL_CONFIG_FIZZ_FROM_PRESET;
	self->fizz_spacing       = GOWL_CONFIG_FIZZ_FROM_PRESET;
	self->fizz_stray         = GOWL_CONFIG_FIZZ_FROM_PRESET;
	self->fizz_cling         = GOWL_CONFIG_FIZZ_FROM_PRESET;
	self->fizz_wobble        = GOWL_CONFIG_FIZZ_FROM_PRESET;
	self->fizz_foam          = GOWL_CONFIG_FIZZ_FROM_PRESET;
	self->fizz_foam_depth    = GOWL_CONFIG_FIZZ_FROM_PRESET;
	self->fizz_depth         = GOWL_CONFIG_FIZZ_FROM_PRESET;
	self->fizz_mirror        = GOWL_CONFIG_FIZZ_FROM_PRESET;
	self->fizz_fog           = GOWL_CONFIG_FIZZ_FROM_PRESET;
	self->fizz_specular      = GOWL_CONFIG_FIZZ_FROM_PRESET;
	self->fizz_speed         = GOWL_CONFIG_FIZZ_FROM_PRESET;

	self->leaves_preset      = g_strdup(GOWL_CONFIG_DEFAULT_LEAVES_PRESET);
	self->leaves_intensity   = GOWL_CONFIG_DEFAULT_LEAVES_INTENSITY;
	self->leaves_fps         = GOWL_CONFIG_DEFAULT_LEAVES_FPS;
	self->leaves_scale       = GOWL_CONFIG_DEFAULT_LEAVES_SCALE;
	self->leaves_warm        = g_strdup(GOWL_CONFIG_DEFAULT_LEAVES_WARM);
	self->leaves_gold        = g_strdup(GOWL_CONFIG_DEFAULT_LEAVES_GOLD);
	self->leaves_dry         = g_strdup(GOWL_CONFIG_DEFAULT_LEAVES_DRY);
	self->leaves_opacity     = GOWL_CONFIG_DEFAULT_LEAVES_OPACITY;
	self->leaves_brightness  = GOWL_CONFIG_DEFAULT_LEAVES_BRIGHTNESS;
	self->leaves_light       = GOWL_CONFIG_DEFAULT_LEAVES_LIGHT;
	self->leaves_frost       = GOWL_CONFIG_DEFAULT_LEAVES_FROST;
	self->leaves_frost_passes = GOWL_CONFIG_DEFAULT_LEAVES_FROST_PASSES;
	self->leaves_leaf        = GOWL_CONFIG_LEAVES_FROM_PRESET;
	self->leaves_cell        = GOWL_CONFIG_LEAVES_FROM_PRESET;
	self->leaves_stuck       = GOWL_CONFIG_LEAVES_FROM_PRESET;
	self->leaves_column      = GOWL_CONFIG_LEAVES_FROM_PRESET;
	self->leaves_falling     = GOWL_CONFIG_LEAVES_FROM_PRESET;
	self->leaves_flutter     = GOWL_CONFIG_LEAVES_FROM_PRESET;
	self->leaves_tumble      = GOWL_CONFIG_LEAVES_FROM_PRESET;
	self->leaves_wind        = GOWL_CONFIG_LEAVES_FROM_PRESET;
	self->leaves_gust        = GOWL_CONFIG_LEAVES_FROM_PRESET;
	self->leaves_gustiness   = GOWL_CONFIG_LEAVES_FROM_PRESET;
	self->leaves_curl        = GOWL_CONFIG_LEAVES_FROM_PRESET;
	self->leaves_veins       = GOWL_CONFIG_LEAVES_FROM_PRESET;
	self->leaves_translucency = GOWL_CONFIG_LEAVES_FROM_PRESET;
	self->leaves_gloss       = GOWL_CONFIG_LEAVES_FROM_PRESET;
	self->leaves_shadow      = GOWL_CONFIG_LEAVES_FROM_PRESET;
	self->leaves_fog         = GOWL_CONFIG_LEAVES_FROM_PRESET;
	self->leaves_speed       = GOWL_CONFIG_LEAVES_FROM_PRESET;
	self->leaves_tenure      = GOWL_CONFIG_LEAVES_FROM_PRESET;

	self->snow_preset        = g_strdup(GOWL_CONFIG_DEFAULT_SNOW_PRESET);
	self->snow_intensity     = GOWL_CONFIG_DEFAULT_SNOW_INTENSITY;
	self->snow_fps           = GOWL_CONFIG_DEFAULT_SNOW_FPS;
	self->snow_scale         = GOWL_CONFIG_DEFAULT_SNOW_SCALE;
	self->snow_tint          = g_strdup(GOWL_CONFIG_DEFAULT_SNOW_TINT);
	self->snow_clarity       = GOWL_CONFIG_DEFAULT_SNOW_CLARITY;
	self->snow_opacity       = GOWL_CONFIG_DEFAULT_SNOW_OPACITY;
	self->snow_brightness    = GOWL_CONFIG_DEFAULT_SNOW_BRIGHTNESS;
	self->snow_light         = GOWL_CONFIG_DEFAULT_SNOW_LIGHT;
	self->snow_frost         = GOWL_CONFIG_DEFAULT_SNOW_FROST;
	self->snow_frost_passes  = GOWL_CONFIG_DEFAULT_SNOW_FROST_PASSES;
	self->snow_flake         = GOWL_CONFIG_SNOW_FROM_PRESET;
	self->snow_cell          = GOWL_CONFIG_SNOW_FROM_PRESET;
	self->snow_settled       = GOWL_CONFIG_SNOW_FROM_PRESET;
	self->snow_column        = GOWL_CONFIG_SNOW_FROM_PRESET;
	self->snow_falling       = GOWL_CONFIG_SNOW_FROM_PRESET;
	self->snow_arms          = GOWL_CONFIG_SNOW_FROM_PRESET;
	self->snow_drift         = GOWL_CONFIG_SNOW_FROM_PRESET;
	self->snow_flutter       = GOWL_CONFIG_SNOW_FROM_PRESET;
	self->snow_spin          = GOWL_CONFIG_SNOW_FROM_PRESET;
	self->snow_melt          = GOWL_CONFIG_SNOW_FROM_PRESET;
	self->snow_shrink        = GOWL_CONFIG_SNOW_FROM_PRESET;
	self->snow_depth         = GOWL_CONFIG_SNOW_FROM_PRESET;
	self->snow_runs          = GOWL_CONFIG_SNOW_FROM_PRESET;
	self->snow_run_width     = GOWL_CONFIG_SNOW_FROM_PRESET;
	self->snow_run_length    = GOWL_CONFIG_SNOW_FROM_PRESET;
	self->snow_beads         = GOWL_CONFIG_SNOW_FROM_PRESET;
	self->snow_ice           = GOWL_CONFIG_SNOW_FROM_PRESET;
	self->snow_ice_rate      = GOWL_CONFIG_SNOW_FROM_PRESET;
	self->snow_ice_scale     = GOWL_CONFIG_SNOW_FROM_PRESET;
	self->snow_sparkle       = GOWL_CONFIG_SNOW_FROM_PRESET;
	self->snow_fog           = GOWL_CONFIG_SNOW_FROM_PRESET;
	self->snow_glow          = GOWL_CONFIG_SNOW_FROM_PRESET;
	self->snow_specular      = GOWL_CONFIG_SNOW_FROM_PRESET;
	self->snow_speed         = GOWL_CONFIG_SNOW_FROM_PRESET;

	self->hints_keys           = g_strdup(GOWL_CONFIG_DEFAULT_HINTS_KEYS);
	self->hints_colors         = g_strdup(GOWL_CONFIG_DEFAULT_HINTS_COLORS);
	self->hints_timeout        = GOWL_CONFIG_DEFAULT_HINTS_TIMEOUT;
	self->hints_size           = GOWL_CONFIG_DEFAULT_HINTS_SIZE;
	self->hints_border_width   = GOWL_CONFIG_DEFAULT_HINTS_BORDER_WIDTH;
	self->hints_scrim          = GOWL_CONFIG_DEFAULT_HINTS_SCRIM;
	self->hints_warp_pointer   = GOWL_CONFIG_DEFAULT_HINTS_WARP_POINTER;
	self->hints_current_output = GOWL_CONFIG_DEFAULT_HINTS_CURRENT_OUTPUT;

	self->no_fx_apps       = g_strdup(GOWL_CONFIG_DEFAULT_NO_FX_APPS);

	self->wallpaper_fade   = GOWL_CONFIG_DEFAULT_WALLPAPER_FADE;

	self->lock_command     = g_strdup(GOWL_CONFIG_DEFAULT_LOCK_COMMAND);
	self->lock_on_suspend  = GOWL_CONFIG_DEFAULT_LOCK_ON_SUSPEND;
	self->nmaster             = GOWL_CONFIG_DEFAULT_NMASTER;
	self->tag_count           = GOWL_CONFIG_DEFAULT_TAG_COUNT;
	self->repeat_rate         = GOWL_CONFIG_DEFAULT_REPEAT_RATE;
	self->repeat_delay        = GOWL_CONFIG_DEFAULT_REPEAT_DELAY;
	self->sloppyfocus         = GOWL_CONFIG_DEFAULT_SLOPPYFOCUS;
	self->manage_lid          = GOWL_CONFIG_DEFAULT_MANAGE_LID;
	self->xkb_layout          = NULL;
	self->xkb_variant         = NULL;
	self->xkb_model           = NULL;
	self->xkb_options         = NULL;
	self->xkb_rules           = NULL;
	self->xkb_file            = NULL;
	self->idle_timeout        = GOWL_CONFIG_DEFAULT_IDLE_TIMEOUT;
	self->dpms_timeout        = GOWL_CONFIG_DEFAULT_DPMS_TIMEOUT;
	self->allow_tearing       = GOWL_CONFIG_DEFAULT_ALLOW_TEARING;
	self->hdr_unmanaged       = GOWL_CONFIG_DEFAULT_HDR_UNMANAGED;
	self->hdr_advertise_pq    = GOWL_CONFIG_DEFAULT_HDR_ADVERTISE_PQ;
	self->hdr_sdr_white       = GOWL_CONFIG_DEFAULT_HDR_SDR_WHITE;
	self->hdr_encode          = GOWL_CONFIG_DEFAULT_HDR_ENCODE;
	self->hdr_bpc             = GOWL_CONFIG_DEFAULT_HDR_BPC;
	self->focus_on_activate   = g_strdup(GOWL_CONFIG_DEFAULT_FOCUS_ON_ACTIVATE);
	self->input_recording     = GOWL_CONFIG_DEFAULT_INPUT_RECORDING;
	self->input_recording_deny_apps =
		g_strdup(GOWL_CONFIG_DEFAULT_INPUT_RECORDING_DENY_APPS);
	self->terminal            = g_strdup(GOWL_CONFIG_DEFAULT_TERMINAL);
	self->menu                = g_strdup(GOWL_CONFIG_DEFAULT_MENU);
	self->log_level           = g_strdup(GOWL_CONFIG_DEFAULT_LOG_LEVEL);
	self->log_file            = g_strdup(GOWL_CONFIG_DEFAULT_LOG_FILE);
	self->evaluate_gowl_config_with_cmacs =
		GOWL_CONFIG_DEFAULT_EVALUATE_GOWL_CONFIG_WITH_CMACS;
	self->evaluate_c_config_with_cmacs =
		GOWL_CONFIG_DEFAULT_EVALUATE_C_CONFIG_WITH_CMACS;

	self->keybinds = g_array_new(FALSE, TRUE, sizeof(GowlKeybindEntry));
	g_array_set_clear_func(self->keybinds, gowl_keybind_entry_clear);
	self->mousebinds = g_array_new(FALSE, TRUE, sizeof(GowlMousebindEntry));
	g_array_set_clear_func(self->mousebinds, gowl_mousebind_entry_clear);
	self->gestures = g_array_new(FALSE, TRUE, sizeof(GowlGestureEntry));
	g_array_set_clear_func(self->gestures, gowl_gesture_entry_clear);
	self->input_configs = g_ptr_array_new_with_free_func(
		gowl_input_config_entry_free);
	gowl_config_add_default_mousebinds(self);

	self->rules = g_ptr_array_new_with_free_func(gowl_rule_entry_free);
	self->dropdowns = g_ptr_array_new_with_free_func(
		gowl_dropdown_entry_free);

	/* Module configs: outer table maps module name -> inner table,
	 * inner table maps setting key -> string value.
	 * Both keys and values are owned (g_free). */
	self->module_configs = g_hash_table_new_full(
		g_str_hash, g_str_equal,
		g_free, (GDestroyNotify)g_hash_table_unref);

	/* Monitor configs: maps output name -> heap-allocated
	 * GowlMonitorConfig*.  Values are plain structs with no inner
	 * pointers, so g_free is sufficient as the destroy func. */
	self->monitor_configs = g_hash_table_new_full(
		g_str_hash, g_str_equal, g_free, g_free);
}

/* --- Public API --- */

/**
 * gowl_config_new:
 *
 * Creates a new #GowlConfig populated with default values.
 *
 * Returns: (transfer full): a new #GowlConfig
 */
GowlConfig *
gowl_config_new(void)
{
	return (GowlConfig *)g_object_new(GOWL_TYPE_CONFIG, NULL);
}

/* --- YAML loading helpers --- */

/* Transform names indexed by wl_output_transform value.  Matches
 * the dictionary used by cmacs's eval-dispatch transform_names[],
 * so the YAML schema, the JSON emitted by cmacs-gowl-list-monitors,
 * and the symbols accepted by `(gowl-set-monitor-transform)` are all
 * symmetric. */
static const gchar *const gowl_monitor_transform_names[] = {
	"normal",       /* 0 */
	"90",           /* 1 */
	"180",          /* 2 */
	"270",          /* 3 */
	"flipped",      /* 4 */
	"flipped-90",   /* 5 */
	"flipped-180",  /* 6 */
	"flipped-270"   /* 7 */
};

/**
 * output_profile_free:
 * @data: a #GowlOutputProfile
 *
 * Destroy function for the profiles list.
 */
static void
output_profile_free(gpointer data)
{
	GowlOutputProfile *p = (GowlOutputProfile *)data;

	if (p == NULL)
		return;
	g_free(p->name);
	g_clear_pointer(&p->outputs, g_hash_table_unref);
	g_free(p);
}

/**
 * parse_monitor_config:
 * @mon_cfg_map: one output's mapping, from `monitors:` or a profile
 *
 * Reads the per-output keys; unset fields keep their sentinels.
 *
 * Returns: (transfer full): a new #GowlMonitorConfig
 */
static GowlMonitorConfig *
parse_monitor_config(YamlMapping *mon_cfg_map)
{
	GowlMonitorConfig *mc = g_new0(GowlMonitorConfig, 1);

	gowl_monitor_config_init(mc);
	if (mon_cfg_map == NULL)
		return mc;

	if (yaml_mapping_has_member(mon_cfg_map, "width"))
		mc->width = (gint)yaml_mapping_get_int_member(
			mon_cfg_map, "width");
	if (yaml_mapping_has_member(mon_cfg_map, "height"))
		mc->height = (gint)yaml_mapping_get_int_member(
			mon_cfg_map, "height");
	if (yaml_mapping_has_member(mon_cfg_map, "refresh"))
		mc->refresh = yaml_mapping_get_double_member(
			mon_cfg_map, "refresh");
	if (yaml_mapping_has_member(mon_cfg_map, "x"))
		mc->x = (gint)yaml_mapping_get_int_member(
			mon_cfg_map, "x");
	if (yaml_mapping_has_member(mon_cfg_map, "y"))
		mc->y = (gint)yaml_mapping_get_int_member(
			mon_cfg_map, "y");
	if (yaml_mapping_has_member(mon_cfg_map, "scale"))
		mc->scale = yaml_mapping_get_double_member(
			mon_cfg_map, "scale");
	if (yaml_mapping_has_member(mon_cfg_map, "enabled"))
		mc->enabled = yaml_mapping_get_boolean_member(
			mon_cfg_map, "enabled") ? 1 : 0;
	if (yaml_mapping_has_member(mon_cfg_map, "transform"))
		mc->transform = gowl_parse_monitor_transform(
			mon_cfg_map);
	/* `vrr' takes a bool or the string "on-demand":
	 * adaptive sync only while a fullscreen game or
	 * video is up, which is the mode that does not
	 * make the cursor stutter on the desktop. */
	if (yaml_mapping_has_member(mon_cfg_map, "vrr")) {
		const gchar *vs = yaml_mapping_get_string_member(
			mon_cfg_map, "vrr");
		if (vs != NULL
		    && (g_ascii_strcasecmp(vs, "on-demand") == 0
		        || g_ascii_strcasecmp(vs, "on_demand") == 0))
			mc->vrr = 2;
		else
			mc->vrr = yaml_mapping_get_boolean_member(
				mon_cfg_map, "vrr") ? 1 : 0;
	}
	/* HDR is a plain bool: an output either is driven in BT.2020 + PQ
	 * or it is not.  Asking for it on a display that cannot do it is
	 * reported when it is applied, not here -- the config is read
	 * before any output exists. */
	if (yaml_mapping_has_member(mon_cfg_map, "hdr"))
		mc->hdr = yaml_mapping_get_boolean_member(mon_cfg_map, "hdr")
		          ? 1 : 0;
	return mc;
}

/**
 * gowl_parse_percent:
 * @text: (nullable): the raw scalar, "0.5" or "50%"
 *
 * A fraction written either way.  yaml-glib keeps a scalar's text, so
 * a percentage and a fraction arrive identically and one reader takes
 * both.  Out-of-range values are clamped rather than refused: a rule
 * asking for 150% of the screen means "as wide as it goes".
 *
 * Returns: the fraction, 0.0 when unparseable (which reads as "unset")
 */
static gdouble
gowl_parse_percent(const gchar *text)
{
	gdouble v;
	gchar *end = NULL;

	if (text == NULL || *text == '\0')
		return 0.0;
	v = g_ascii_strtod(text, &end);
	if (end != NULL && *end == '%')
		v /= 100.0;
	if (v < 0.0)
		return 0.0;
	return v > 1.0 ? 1.0 : v;
}

/**
 * gowl_parse_monitor_transform:
 * @cm: a #YamlMapping describing one monitor's config
 *
 * Reads the `transform:` member and returns its
 * wl_output_transform code (0..7).  Accepts either an integer
 * 0..7 (`transform: 3`) or one of the canonical names listed
 * above (`transform: flipped-270`).  Names that happen to look
 * like integers (e.g. `90`, `180`, `270`) are accepted via the
 * name table when the numeric parse is out of range.  Returns -1
 * and warns on miss or unparseable input -- the field is optional.
 */
static gint
gowl_parse_monitor_transform(YamlMapping *cm)
{
	const gchar *raw;
	gchar *end;
	gint64 num;
	gsize i;

	raw = yaml_mapping_get_string_member(cm, "transform");
	if (raw == NULL)
		return -1;

	/* Try integer 0..7 first -- the most common case for users
	 * who learn the codes from wl_output_transform docs. */
	end = NULL;
	num = g_ascii_strtoll(raw, &end, 10);
	if (end != raw && *end == '\0' && num >= 0 && num <= 7)
		return (gint)num;

	/* Fall through to the name table.  This also catches
	 * "90"/"180"/"270" -- those are canonical *names* (degrees of
	 * rotation), not transform codes, but users write them
	 * intuitively and the table maps them to the right codes. */
	for (i = 0; i < G_N_ELEMENTS(gowl_monitor_transform_names); i++) {
		if (g_strcmp0(raw, gowl_monitor_transform_names[i]) == 0)
			return (gint)i;
	}

	g_warning("gowl_config: invalid transform '%s' (expected 0..7 "
	          "or one of: normal, 90, 180, 270, flipped, flipped-90, "
	          "flipped-180, flipped-270)", raw);
	return -1;
}

/*
 * Reads a `palette:' block and rebuilds the effective palette.
 *
 *   palette:
 *     name: latte          # a built-in to start from
 *     accent: "#d20f39"    # anything else overrides one entry
 *
 * `name' is consumed rather than stored as an entry, so a palette
 * cannot accidentally define a colour called "name".  A config with no
 * palette block still gets one --- the default flavour --- so a colour
 * key naming `accent' resolves in every config, including one written
 * before palettes existed.
 */
static void
gowl_config_apply_palette_mapping(GowlConfig *self, YamlMapping *mapping)
{
	g_autoptr(GowlPalette) from_file = NULL;
	YamlNode *node;
	YamlMapping *pal_map;
	guint i, count;

	from_file = gowl_palette_new();

	if (!yaml_mapping_has_member(mapping, "palette")) {
		gowl_config_rebuild_palette(self, from_file);
		return;
	}

	node = yaml_mapping_get_member(mapping, "palette");
	pal_map = node != NULL ? yaml_node_get_mapping(node) : NULL;
	if (pal_map == NULL) {
		g_warning("gowl_config: `palette:' is not a mapping, ignoring");
		gowl_config_rebuild_palette(self, from_file);
		return;
	}

	count = yaml_mapping_get_size(pal_map);
	for (i = 0; i < count; i++) {
		const gchar *key;
		YamlNode *val_node;
		const gchar *val;

		key = yaml_mapping_get_key(pal_map, i);
		val_node = yaml_mapping_get_value(pal_map, i);
		if (key == NULL || val_node == NULL)
			continue;

		val = yaml_node_get_scalar(val_node);
		if (val == NULL)
			continue;

		if (g_strcmp0(key, "name") == 0) {
			g_free(self->palette_name);
			self->palette_name = g_strdup(val);
			continue;
		}

		gowl_palette_set(from_file, key, val);
	}

	gowl_config_rebuild_palette(self, from_file);
}

/* -----------------------------------------------------------
 * Validation: the keys each section knows
 *
 * A typo in a key used to be silent: the parser asked for the keys it
 * knew and never looked at the rest.  Each section now checks its
 * mapping against a list, warns for anything else -- naming the known
 * key it is closest to, when one is close -- and counts it, so that
 * `gowl --check-config' can fail and a reload can say how many.
 * tests/test-config-keys.sh keeps the top-level list complete: every
 * key the parser asks for must be in it.
 * ----------------------------------------------------------- */

static const gchar *const top_level_keys[] = {
	"ignore_yaml", "log-level", "log-file", "repeat-rate", "repeat-delay",
	"terminal", "menu", "sloppyfocus", "manage_lid", "idle-timeout",
	"dpms-timeout", "allow-tearing", "hdr-unmanaged", "hdr-advertise-pq", "hdr-sdr-white",
	"hdr-encode", "hdr-bpc",
	"focus-on-activate",
	"input-recording", "input-recording-deny-apps",
	"evaluate_gowl_config_with_cmacs", "evaluate-gowl-config-with-cmacs",
	"evaluate_c_config_with_cmacs", "evaluate-c-config-with-cmacs",
	"xkb-layout", "xkb-variant", "xkb-model", "xkb-options", "xkb-rules",
	"xkb-file", "palette", "border-width", "border-color-focus",
	"border-color-unfocus", "border-color-urgent", "mfact", "nmaster",
	"tag-count", "scroll-column-width", "animations", "animation-duration",
	"animation-duration-open", "animation-duration-close",
	"animation-curve-open", "animation-curve-close", "animation-curve",
	"animation-popin-scale",
	"animation-jiggle-strength", "cube", "cube-duration",
	"cube-step-duration", "cube-curve", "cube-faces", "cube-zoom",
	"cube-pitch", "cube-shading", "cube-reflection", "cube-motion-blur",
	"cube-backdrop-color", "cube-caps", "cube-all-monitors", "cube-gesture",
	"magnifier", "magnifier-max", "magnifier-step", "magnifier-smoothing",
	"magnifier-follow-cursor", "magnifier-smooth", "magnifier-modifier",
	"expo", "expo-duration", "expo-curve", "expo-tags", "expo-columns",
	"expo-gap", "expo-corner", "expo-dim", "expo-hide-empty",
	"expo-backdrop-color", "switcher", "switcher-duration", "switcher-curve",
	"switcher-scale", "switcher-spacing", "switcher-angle",
	"switcher-reflection", "switcher-all-tags", "switcher-backdrop-color",
	"blur", "blur-downscale", "blur-passes", "blur-brightness", "shadow",
	"window-backdrop", "glass-bevel", "glass-thickness", "glass-slope",
	"glass-shape", "glass-dispersion", "glass-rim", "glass-shade",
	"glass-edge-width", "glass-saturation", "glass-clarity", "glass-light",
	"glass-centre-clarity", "glass-lens", "glass-sheen",
	"glass-tint", "glass-brightness", "glass-opacity", "glass-frost",
	"glass-frost-passes",
	"water-preset", "water-intensity", "water-fps", "water-scale",
	"water-tint", "water-clarity", "water-opacity", "water-brightness",
	"water-light", "water-frost", "water-frost-passes",
	"water-amplitude", "water-wavelength", "water-choppiness",
	"water-depth", "water-drops", "water-shore", "water-specular",
	"water-caustics", "water-foam", "water-fresnel", "water-speed",
	"rain-preset", "rain-intensity", "rain-fps", "rain-scale",
	"rain-tint", "rain-clarity", "rain-opacity", "rain-brightness",
	"rain-light", "rain-frost", "rain-frost-passes",
	"rain-cell", "rain-density", "rain-bulge", "rain-depth",
	"rain-runs", "rain-run-width", "rain-run-length", "rain-beads",
	"rain-fog", "rain-specular", "rain-impact", "rain-speed",
	"rain-lightning", "rain-lightning-rate", "rain-lightning-power",
	"soap-preset", "soap-intensity", "soap-fps", "soap-scale",
	"soap-tint", "soap-clarity", "soap-opacity", "soap-brightness",
	"soap-light", "soap-frost", "soap-frost-passes", "soap-thickness",
	"soap-thin", "soap-drain", "soap-turbulence", "soap-swirl",
	"soap-index", "soap-gain", "soap-sheen", "soap-wedge", "soap-pop",
	"soap-meniscus", "soap-fog", "soap-speed", "soap-life",

	"embers-preset", "embers-intensity", "embers-fps", "embers-scale",
	"embers-tint", "embers-clarity", "embers-opacity",
	"embers-brightness", "embers-frost", "embers-frost-passes",
	"embers-column", "embers-density", "embers-ember", "embers-spacing",
	"embers-sway", "embers-drag", "embers-temperature", "embers-cool",
	"embers-flicker", "embers-ash", "embers-glow", "embers-hearth",
	"embers-haze", "embers-haze-scale", "embers-fog", "embers-speed",

	"submerged-preset", "submerged-intensity", "submerged-fps",
	"submerged-scale", "submerged-water", "submerged-clarity",
	"submerged-opacity", "submerged-brightness", "submerged-frost",
	"submerged-frost-passes", "submerged-depth", "submerged-extinction-r",
	"submerged-extinction-g", "submerged-extinction-b", "submerged-murk",
	"submerged-caustics", "submerged-caustic-scale", "submerged-shafts",
	"submerged-shaft-lean", "submerged-motes", "submerged-mote-size",
	"submerged-surface", "submerged-sway", "submerged-fog",
	"submerged-speed", "submerged-drift",

	"dew-preset", "dew-intensity", "dew-fps", "dew-scale", "dew-tint",
	"dew-clarity", "dew-opacity", "dew-brightness", "dew-light",
	"dew-frost", "dew-frost-passes", "dew-radials", "dew-pitch",
	"dew-thread", "dew-drop", "dew-spacing", "dew-sag", "dew-depth",
	"dew-bulge", "dew-silk", "dew-glint", "dew-shine", "dew-rim",
	"dew-sway", "dew-fog", "dew-speed",

	"bokeh-radius", "bokeh-downscale", "bokeh-samples", "bokeh-blades",
	"bokeh-rotation", "bokeh-highlight", "bokeh-threshold", "bokeh-edge",
	"bokeh-brightness",
	"fizz-preset", "fizz-intensity", "fizz-fps", "fizz-scale", "fizz-tint",
	"fizz-clarity", "fizz-opacity", "fizz-brightness", "fizz-light",
	"fizz-frost", "fizz-frost-passes", "fizz-cell", "fizz-bubble",
	"fizz-growth", "fizz-sites", "fizz-site-width", "fizz-spacing",
	"fizz-stray", "fizz-cling", "fizz-wobble", "fizz-foam",
	"fizz-foam-depth", "fizz-depth", "fizz-mirror", "fizz-fog",
	"fizz-specular", "fizz-speed", "leaves-preset", "leaves-intensity",
	"leaves-fps", "leaves-scale", "leaves-warm", "leaves-gold",
	"leaves-dry", "leaves-opacity", "leaves-brightness", "leaves-light",
	"leaves-frost", "leaves-frost-passes", "leaves-leaf", "leaves-cell",
	"leaves-stuck", "leaves-column", "leaves-falling", "leaves-flutter",
	"leaves-tumble", "leaves-wind", "leaves-gust", "leaves-gustiness",
	"leaves-curl", "leaves-veins", "leaves-translucency", "leaves-gloss",
	"leaves-shadow", "leaves-fog", "leaves-speed", "leaves-tenure",
	"snow-preset", "snow-intensity", "snow-fps", "snow-scale", "snow-tint",
	"snow-clarity", "snow-opacity", "snow-brightness", "snow-light",
	"snow-frost", "snow-frost-passes", "snow-flake", "snow-cell",
	"snow-settled", "snow-column", "snow-falling", "snow-arms",
	"snow-drift", "snow-flutter", "snow-spin", "snow-melt", "snow-shrink",
	"snow-depth", "snow-runs", "snow-run-width", "snow-run-length",
	"snow-beads", "snow-ice", "snow-ice-rate", "snow-ice-scale",
	"snow-sparkle", "snow-fog", "snow-glow", "snow-specular", "snow-speed",
	"hints-keys", "hints-colors", "hints-timeout", "hints-size",
	"hints-border-width", "hints-scrim", "hints-warp-pointer",
	"hints-current-output",
	"no-fx-apps",
	"shadow-radius", "shadow-opacity", "shadow-offset-x", "shadow-offset-y",
	"shadow-color", "wallpaper-fade", "wallpaper-tags", "wallpaper-outputs",
	"lock-command", "lock-on-suspend", "keybinds", "modes",
	"mousebinds", "gestures", "input", "rules", "dropdowns", "autostart",
	"monitors", "modules", "profiles",
	NULL
};

static const gchar *const rule_keys[] = {
	"app-id", "app_id", "title", "tags", "floating", "monitor", "width",
	"height", "center", "regex", "sticky", "initial-title", "xwayland",
	"pid", "no-focus", "fullscreen", "opacity", "no-blur", "no-shadow",
	"no-anim", "idle-inhibit", "is-floating", "is-fullscreen", "on-tag",
	"focus", "width-pct", "height-pct", NULL
};

static const gchar *const bind_keys[] = {
	"action", "arg", "desc", "mode", "locked", "release", "repeat", NULL
};

static const gchar *const monitor_keys[] = {
	"width", "height", "refresh", "x", "y", "scale", "enabled",
	"transform", "vrr", "hdr", NULL
};

static const gchar *const input_keys[] = {
	"enabled", "tap", "tap-drag", "tap-drag-lock", "tap-button-map",
	"natural-scroll", "scroll-method", "scroll-button", "click-method",
	"accel-profile", "accel-speed", "left-handed", "middle-emulation",
	"dwt", "dwtp", "rotation", NULL
};

/* Edit distance, for "did you mean". */
static guint
edit_distance(const gchar *a, const gchar *b)
{
	gsize la = strlen(a), lb = strlen(b), i, j;
	guint *row = g_new(guint, lb + 1);
	guint result;

	for (j = 0; j <= lb; j++)
		row[j] = (guint)j;
	for (i = 1; i <= la; i++) {
		guint prev = row[0];

		row[0] = (guint)i;
		for (j = 1; j <= lb; j++) {
			guint tmp = row[j];
			guint cost = (a[i - 1] == b[j - 1]) ? 0 : 1;

			row[j] = MIN(MIN(row[j] + 1, row[j - 1] + 1), prev + cost);
			prev = tmp;
		}
	}
	result = row[lb];
	g_free(row);
	return result;
}

/* Warns about every key of @mapping not in @known, counting each. */
static void
check_known_keys(
	GowlConfig         *self,
	YamlMapping        *mapping,
	const gchar        *section,
	const gchar *const *known
){
	guint n = yaml_mapping_get_size(mapping);
	guint i;

	for (i = 0; i < n; i++) {
		const gchar *key = yaml_mapping_get_key(mapping, i);
		const gchar *best = NULL;
		guint best_d = 4;
		gsize k;
		gboolean found = FALSE;

		if (key == NULL)
			continue;
		for (k = 0; known[k] != NULL; k++) {
			guint d;

			if (g_strcmp0(known[k], key) == 0) {
				found = TRUE;
				break;
			}
			d = edit_distance(known[k], key);
			if (d < best_d) {
				best_d = d;
				best = known[k];
			}
		}
		if (found)
			continue;
		self->problems++;
		if (best != NULL)
			g_warning("gowl_config: %s: unknown key '%s' (did you mean "
			          "'%s'?)", section, key, best);
		else
			g_warning("gowl_config: %s: unknown key '%s'", section, key);
	}
}

/**
 * parse_output_profiles:
 * @self: the config
 * @mapping: the document root
 *
 * Reads `profiles:`, a mapping of profile name to a mapping of output
 * key to the same keys `monitors:` takes.  Replaces the list on a
 * reload.
 */
static void
parse_output_profiles(GowlConfig *self, YamlMapping *mapping)
{
	YamlMapping *profiles_map;
	guint count;
	guint i;

	g_list_free_full(self->profiles, output_profile_free);
	self->profiles = NULL;
	if (!yaml_mapping_has_member(mapping, "profiles"))
		return;
	profiles_map = yaml_mapping_get_mapping_member(mapping, "profiles");
	if (profiles_map == NULL)
		return;

	count = yaml_mapping_get_size(profiles_map);
	for (i = 0; i < count; i++) {
		const gchar *pname = yaml_mapping_get_key(profiles_map, i);
		YamlNode *pnode = yaml_mapping_get_value(profiles_map, i);
		YamlMapping *outputs;
		GowlOutputProfile *profile;
		guint n_outputs;
		guint j;

		if (pname == NULL || pnode == NULL)
			continue;
		outputs = yaml_node_get_mapping(pnode);
		if (outputs == NULL) {
			g_warning("gowl_config: profiles.%s: expected a mapping of "
			          "outputs", pname);
			self->problems++;
			continue;
		}
		profile = g_new0(GowlOutputProfile, 1);
		profile->name = g_strdup(pname);
		profile->outputs = g_hash_table_new_full(g_str_hash, g_str_equal,
		                                         g_free, g_free);
		n_outputs = yaml_mapping_get_size(outputs);
		for (j = 0; j < n_outputs; j++) {
			const gchar *okey = yaml_mapping_get_key(outputs, j);
			YamlNode *onode = yaml_mapping_get_value(outputs, j);
			YamlMapping *omap;

			if (okey == NULL || onode == NULL)
				continue;
			omap = yaml_node_get_mapping(onode);
			if (omap == NULL) {
				/* `eDP-1: {}` and a bare `eDP-1:` both mean "must be
				 * present, leave it as it is". */
				g_hash_table_insert(profile->outputs, g_strdup(okey),
				                    parse_monitor_config(NULL));
				continue;
			}
			check_known_keys(self, omap, "profiles", monitor_keys);
			g_hash_table_insert(profile->outputs, g_strdup(okey),
			                    parse_monitor_config(omap));
		}
		if (g_hash_table_size(profile->outputs) == 0) {
			g_warning("gowl_config: profiles.%s names no outputs", pname);
			self->problems++;
		}
		self->profiles = g_list_append(self->profiles, profile);
	}
}


guint
gowl_config_get_problem_count(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self), 0);
	return self->problems;
}

/* -----------------------------------------------------------
 * Shared pieces of a bind entry in YAML
 * ----------------------------------------------------------- */

/* The `action', `arg' and `desc' of a bind mapping.  Resolves the
 * action nick (underscores accepted for hyphens).  Returns FALSE, with
 * a warning, when there is no usable action. */
static gboolean
yaml_action_entry(
	GowlConfig   *self,
	YamlMapping  *val_map,
	gint         *out_action,
	const gchar **out_arg,
	const gchar **out_desc
){
	const gchar *action_str;
	GEnumClass *action_class;
	GEnumValue *enum_val;
	g_autofree gchar *norm = NULL;

	action_str = yaml_mapping_get_string_member(val_map, "action");
	if (action_str == NULL) {
		g_warning("gowl_config: bind without an action");
		return FALSE;
	}
	action_class = (GEnumClass *)g_type_class_ref(gowl_action_get_type());
	norm = g_strdup(action_str);
	g_strdelimit(norm, "_", '-');
	enum_val = g_enum_get_value_by_nick(action_class, norm);
	g_type_class_unref(action_class);
	if (enum_val == NULL) {
		g_warning("gowl_config: unknown action '%s'", action_str);
		self->problems++;
		return FALSE;
	}
	check_known_keys(self, val_map, "bind", bind_keys);
	*out_action = enum_val->value;
	*out_arg = yaml_mapping_has_member(val_map, "arg")
	           ? yaml_mapping_get_string_member(val_map, "arg") : NULL;
	*out_desc = yaml_mapping_has_member(val_map, "desc")
	            ? yaml_mapping_get_string_member(val_map, "desc") : NULL;
	return TRUE;
}

/* A bind's `mode', or the mode of the section it sits in. */
static const gchar *
yaml_keybind_mode(YamlMapping *val_map, const gchar *section_mode)
{
	if (yaml_mapping_has_member(val_map, "mode"))
		return yaml_mapping_get_string_member(val_map, "mode");
	return section_mode;
}

/* `locked', `release' and `repeat' as #GowlKeybindFlags. */
static guint
yaml_keybind_flags(YamlMapping *val_map)
{
	guint flags = GOWL_KEYBIND_FLAG_NONE;

	if (yaml_mapping_has_member(val_map, "locked")
	    && yaml_mapping_get_boolean_member(val_map, "locked"))
		flags |= GOWL_KEYBIND_FLAG_LOCKED;
	if (yaml_mapping_has_member(val_map, "release")
	    && yaml_mapping_get_boolean_member(val_map, "release"))
		flags |= GOWL_KEYBIND_FLAG_RELEASE;
	if (yaml_mapping_has_member(val_map, "repeat")
	    && !yaml_mapping_get_boolean_member(val_map, "repeat"))
		flags |= GOWL_KEYBIND_FLAG_NO_REPEAT;
	return flags;
}

/* A keybinds mapping whose every bind belongs to @mode: the body of
 * one entry of `modes:'. */
static void
gowl_config_load_keybind_mapping(
	GowlConfig  *self,
	YamlMapping *binds,
	const gchar *mode
){
	guint count = yaml_mapping_get_size(binds);
	guint i;

	for (i = 0; i < count; i++) {
		const gchar *bind_str = yaml_mapping_get_key(binds, i);
		YamlNode *val_node = yaml_mapping_get_value(binds, i);
		YamlMapping *val_map;
		gint action;
		const gchar *arg_str, *desc_str;
		guint mods, keysym;

		if (bind_str == NULL || val_node == NULL)
			continue;
		val_map = yaml_node_get_mapping(val_node);
		if (val_map == NULL)
			continue;
		if (!yaml_action_entry(self, val_map, &action, &arg_str, &desc_str))
			continue;
		if (!gowl_keybind_parse(bind_str, &mods, &keysym)) {
			g_warning("gowl_config: failed to parse keybind '%s' "
			          "in mode '%s'", bind_str, mode);
			continue;
		}
		gowl_config_add_keybind_ex(self, mods, keysym, action, arg_str,
		                           desc_str,
		                           yaml_keybind_mode(val_map, mode),
		                           yaml_keybind_flags(val_map));
	}
}

/**
 * gowl_config_apply_mapping:
 * @self: a #GowlConfig
 * @mapping: a #YamlMapping containing top-level config keys
 *
 * Walks the YAML mapping and applies recognised keys to the
 * corresponding GObject properties. Unrecognised keys are logged
 * as warnings and skipped.
 */
static void
gowl_config_apply_mapping(
	GowlConfig  *self,
	YamlMapping *mapping
){
	/* Scalar properties: read from the mapping if present */
	/*
	 * The palette is applied before anything else in the mapping,
	 * regardless of where it appears in the file.  Colour keys resolve
	 * against it as they are read, so a `palette:' block written at the
	 * bottom of a config would otherwise silently do nothing to the
	 * keys above it --- a failure with no error and a plausible result.
	 */
	gowl_config_apply_palette_mapping(self, mapping);

	/* Everything the sections below do not know is reported here; the
	 * count is what --check-config and a reload's message read. */
	self->problems = 0;
	check_known_keys(self, mapping, "config", top_level_keys);

	if (yaml_mapping_has_member(mapping, "border-width")) {
		gint64 val = yaml_mapping_get_int_member(mapping, "border-width");
		g_object_set(self, "border-width", (gint)val, NULL);
	}
	if (yaml_mapping_has_member(mapping, "border-color-focus")) {
		const gchar *val = yaml_mapping_get_string_member(mapping, "border-color-focus");
		if (val != NULL)
			g_object_set(self, "border-color-focus", val, NULL);
	}
	if (yaml_mapping_has_member(mapping, "border-color-unfocus")) {
		const gchar *val = yaml_mapping_get_string_member(mapping, "border-color-unfocus");
		if (val != NULL)
			g_object_set(self, "border-color-unfocus", val, NULL);
	}
	if (yaml_mapping_has_member(mapping, "border-color-urgent")) {
		const gchar *val = yaml_mapping_get_string_member(mapping, "border-color-urgent");
		if (val != NULL)
			g_object_set(self, "border-color-urgent", val, NULL);
	}
	if (yaml_mapping_has_member(mapping, "animations")) {
		self->animations = yaml_mapping_get_boolean_member(
			mapping, "animations");
	}
	if (yaml_mapping_has_member(mapping, "animation-duration")) {
		self->animation_duration = (gint)yaml_mapping_get_int_member(
			mapping, "animation-duration");
	}
	if (yaml_mapping_has_member(mapping, "animation-duration-open")) {
		self->animation_duration_open =
			(gint)yaml_mapping_get_int_member(
				mapping, "animation-duration-open");
	}
	if (yaml_mapping_has_member(mapping, "animation-duration-close")) {
		self->animation_duration_close =
			(gint)yaml_mapping_get_int_member(
				mapping, "animation-duration-close");
	}
	if (yaml_mapping_has_member(mapping, "animation-curve-open")) {
		const gchar *v = yaml_mapping_get_string_member(mapping, "animation-curve-open");

		if (v != NULL) {
			g_free(self->animation_curve_open);
			self->animation_curve_open = g_strdup(v);
		}
	}
	if (yaml_mapping_has_member(mapping, "animation-curve-close")) {
		const gchar *v = yaml_mapping_get_string_member(mapping, "animation-curve-close");

		if (v != NULL) {
			g_free(self->animation_curve_close);
			self->animation_curve_close = g_strdup(v);
		}
	}
	if (yaml_mapping_has_member(mapping, "animation-popin-scale")) {
		gdouble v = yaml_mapping_get_double_member(mapping, "animation-popin-scale");

		/* Keep invalid input from collapsing or inverting a window. */
		if (v >= 0.5 && v <= 1.0)
			self->animation_popin_scale = v;
	}
	if (yaml_mapping_has_member(mapping, "animation-jiggle-strength")) {
		gdouble v = yaml_mapping_get_double_member(mapping, "animation-jiggle-strength");

		if (v >= 0.0 && v <= 2.0)
			self->animation_jiggle_strength = v;
	}

	/* Desktop cube.  Every numeric key clamps rather than rejecting so a
	 * plausible-but-out-of-range value still does something sensible ---
	 * except the durations, where a negative would run the rotation
	 * backwards in time, so those are rejected outright. */
	if (yaml_mapping_has_member(mapping, "cube")) {
		self->cube = yaml_mapping_get_boolean_member(mapping, "cube");
	}
	if (yaml_mapping_has_member(mapping, "cube-duration")) {
		gint v = (gint)yaml_mapping_get_int_member(mapping,
		                                           "cube-duration");
		if (v >= 0)
			self->cube_duration = v;
	}
	if (yaml_mapping_has_member(mapping, "cube-step-duration")) {
		gint v = (gint)yaml_mapping_get_int_member(mapping,
		                                           "cube-step-duration");
		if (v >= 0)
			self->cube_step_duration = v;
	}
	if (yaml_mapping_has_member(mapping, "cube-curve")) {
		const gchar *v = yaml_mapping_get_string_member(mapping,
		                                                "cube-curve");
		if (v != NULL) {
			g_free(self->cube_curve);
			self->cube_curve = g_strdup(v);
		}
	}
	if (yaml_mapping_has_member(mapping, "cube-faces")) {
		gint v = (gint)yaml_mapping_get_int_member(mapping, "cube-faces");
		self->cube_faces = CLAMP(v, 3, 12);
	}
	if (yaml_mapping_has_member(mapping, "cube-zoom")) {
		self->cube_zoom = CLAMP(yaml_mapping_get_double_member(
			mapping, "cube-zoom"), 1.0, 3.0);
	}
	if (yaml_mapping_has_member(mapping, "cube-pitch")) {
		self->cube_pitch = CLAMP(yaml_mapping_get_double_member(
			mapping, "cube-pitch"), -30.0, 30.0);
	}
	if (yaml_mapping_has_member(mapping, "cube-shading")) {
		self->cube_shading = CLAMP(yaml_mapping_get_double_member(
			mapping, "cube-shading"), 0.0, 1.0);
	}
	if (yaml_mapping_has_member(mapping, "cube-reflection")) {
		self->cube_reflection = CLAMP(yaml_mapping_get_double_member(
			mapping, "cube-reflection"), 0.0, 1.0);
	}
	if (yaml_mapping_has_member(mapping, "cube-motion-blur")) {
		self->cube_motion_blur = CLAMP(yaml_mapping_get_double_member(
			mapping, "cube-motion-blur"), 0.0, 1.0);
	}
	if (yaml_mapping_has_member(mapping, "cube-backdrop-color")) {
		const gchar *v = yaml_mapping_get_string_member(
			mapping, "cube-backdrop-color");
		if (v != NULL) {
			g_free(self->cube_backdrop_color);
			self->cube_backdrop_color =
				gowl_palette_resolve(self->palette, v);
		}
	}
	if (yaml_mapping_has_member(mapping, "cube-caps")) {
		self->cube_caps = yaml_mapping_get_boolean_member(mapping,
		                                                  "cube-caps");
	}
	if (yaml_mapping_has_member(mapping, "cube-all-monitors")) {
		self->cube_all_monitors = yaml_mapping_get_boolean_member(
			mapping, "cube-all-monitors");
	}
	if (yaml_mapping_has_member(mapping, "cube-gesture")) {
		self->cube_gesture = yaml_mapping_get_boolean_member(
			mapping, "cube-gesture");
	}

	/* Magnifier. */
	if (yaml_mapping_has_member(mapping, "magnifier"))
		self->magnifier = yaml_mapping_get_boolean_member(mapping, "magnifier");
	if (yaml_mapping_has_member(mapping, "magnifier-max")) {
		self->magnifier_max = CLAMP(yaml_mapping_get_double_member(
			mapping, "magnifier-max"), 1.0, 32.0);
	}
	if (yaml_mapping_has_member(mapping, "magnifier-step")) {
		self->magnifier_step = CLAMP(yaml_mapping_get_double_member(
			mapping, "magnifier-step"), 1.01, 4.0);
	}
	if (yaml_mapping_has_member(mapping, "magnifier-smoothing")) {
		gint v = (gint)yaml_mapping_get_int_member(mapping,
		                                           "magnifier-smoothing");
		if (v >= 0)
			self->magnifier_smoothing = v;
	}
	if (yaml_mapping_has_member(mapping, "magnifier-follow-cursor")) {
		self->magnifier_follow_cursor = yaml_mapping_get_boolean_member(
			mapping, "magnifier-follow-cursor");
	}
	if (yaml_mapping_has_member(mapping, "magnifier-smooth")) {
		self->magnifier_smooth = yaml_mapping_get_boolean_member(
			mapping, "magnifier-smooth");
	}
	if (yaml_mapping_has_member(mapping, "magnifier-modifier")) {
		const gchar *v = yaml_mapping_get_string_member(
			mapping, "magnifier-modifier");
		if (v != NULL) {
			g_free(self->magnifier_modifier);
			self->magnifier_modifier = g_strdup(v);
		}
	}

	/* Expo. */
	if (yaml_mapping_has_member(mapping, "expo"))
		self->expo = yaml_mapping_get_boolean_member(mapping, "expo");
	if (yaml_mapping_has_member(mapping, "expo-duration")) {
		gint v = (gint)yaml_mapping_get_int_member(mapping, "expo-duration");
		if (v >= 0)
			self->expo_duration = v;
	}
	if (yaml_mapping_has_member(mapping, "expo-curve")) {
		const gchar *v = yaml_mapping_get_string_member(mapping, "expo-curve");
		if (v != NULL) {
			g_free(self->expo_curve);
			self->expo_curve = g_strdup(v);
		}
	}
	if (yaml_mapping_has_member(mapping, "expo-tags")) {
		self->expo_tags = CLAMP((gint)yaml_mapping_get_int_member(
			mapping, "expo-tags"), 1, GOWL_CONFIG_MAX_TAGS);
	}
	if (yaml_mapping_has_member(mapping, "expo-columns")) {
		self->expo_columns = CLAMP((gint)yaml_mapping_get_int_member(
			mapping, "expo-columns"), 0, GOWL_CONFIG_MAX_TAGS);
	}
	if (yaml_mapping_has_member(mapping, "expo-gap")) {
		self->expo_gap = CLAMP(yaml_mapping_get_double_member(
			mapping, "expo-gap"), 0.0, 0.4);
	}
	if (yaml_mapping_has_member(mapping, "expo-corner")) {
		self->expo_corner = CLAMP(yaml_mapping_get_double_member(
			mapping, "expo-corner"), 0.0, 0.3);
	}
	if (yaml_mapping_has_member(mapping, "expo-dim")) {
		self->expo_dim = CLAMP(yaml_mapping_get_double_member(
			mapping, "expo-dim"), 0.0, 1.0);
	}
	if (yaml_mapping_has_member(mapping, "expo-hide-empty")) {
		self->expo_hide_empty = yaml_mapping_get_boolean_member(
			mapping, "expo-hide-empty");
	}
	if (yaml_mapping_has_member(mapping, "expo-backdrop-color")) {
		const gchar *v = yaml_mapping_get_string_member(
			mapping, "expo-backdrop-color");
		if (v != NULL) {
			g_free(self->expo_backdrop_color);
			self->expo_backdrop_color =
				gowl_palette_resolve(self->palette, v);
		}
	}

	/* Switcher. */
	if (yaml_mapping_has_member(mapping, "switcher"))
		self->switcher = yaml_mapping_get_boolean_member(mapping, "switcher");
	if (yaml_mapping_has_member(mapping, "switcher-duration")) {
		gint v = (gint)yaml_mapping_get_int_member(mapping,
		                                           "switcher-duration");
		if (v >= 0)
			self->switcher_duration = v;
	}
	if (yaml_mapping_has_member(mapping, "switcher-curve")) {
		const gchar *v = yaml_mapping_get_string_member(mapping,
		                                                "switcher-curve");
		if (v != NULL) {
			g_free(self->switcher_curve);
			self->switcher_curve = g_strdup(v);
		}
	}
	if (yaml_mapping_has_member(mapping, "switcher-scale")) {
		self->switcher_scale = CLAMP(yaml_mapping_get_double_member(
			mapping, "switcher-scale"), 0.2, 1.0);
	}
	if (yaml_mapping_has_member(mapping, "switcher-spacing")) {
		self->switcher_spacing = CLAMP(yaml_mapping_get_double_member(
			mapping, "switcher-spacing"), 0.3, 2.0);
	}
	if (yaml_mapping_has_member(mapping, "switcher-angle")) {
		self->switcher_angle = CLAMP(yaml_mapping_get_double_member(
			mapping, "switcher-angle"), 0.0, 80.0);
	}
	if (yaml_mapping_has_member(mapping, "switcher-reflection")) {
		self->switcher_reflection = CLAMP(yaml_mapping_get_double_member(
			mapping, "switcher-reflection"), 0.0, 1.0);
	}
	if (yaml_mapping_has_member(mapping, "switcher-all-tags")) {
		self->switcher_all_tags = yaml_mapping_get_boolean_member(
			mapping, "switcher-all-tags");
	}
	if (yaml_mapping_has_member(mapping, "switcher-backdrop-color")) {
		const gchar *v = yaml_mapping_get_string_member(
			mapping, "switcher-backdrop-color");
		if (v != NULL) {
			g_free(self->switcher_backdrop_color);
			self->switcher_backdrop_color =
				gowl_palette_resolve(self->palette, v);
		}
	}

	/* Blur and shadows. */
	if (yaml_mapping_has_member(mapping, "blur"))
		self->blur = yaml_mapping_get_boolean_member(mapping, "blur");
	if (yaml_mapping_has_member(mapping, "blur-downscale")) {
		self->blur_downscale = CLAMP((gint)yaml_mapping_get_int_member(
			mapping, "blur-downscale"), 1, 8);
	}
	if (yaml_mapping_has_member(mapping, "blur-passes")) {
		self->blur_passes = CLAMP((gint)yaml_mapping_get_int_member(
			mapping, "blur-passes"), 1, 6);
	}
	if (yaml_mapping_has_member(mapping, "blur-brightness")) {
		self->blur_brightness = CLAMP(yaml_mapping_get_double_member(
			mapping, "blur-brightness"), 0.2, 2.0);
	}
	if (yaml_mapping_has_member(mapping, "shadow"))
		self->shadow = yaml_mapping_get_boolean_member(mapping, "shadow");
	if (yaml_mapping_has_member(mapping, "shadow-radius")) {
		self->shadow_radius = CLAMP((gint)yaml_mapping_get_int_member(
			mapping, "shadow-radius"), 0, 128);
	}
	if (yaml_mapping_has_member(mapping, "shadow-opacity")) {
		self->shadow_opacity = CLAMP(yaml_mapping_get_double_member(
			mapping, "shadow-opacity"), 0.0, 1.0);
	}
	if (yaml_mapping_has_member(mapping, "shadow-offset-x")) {
		self->shadow_offset_x = CLAMP((gint)yaml_mapping_get_int_member(
			mapping, "shadow-offset-x"), -128, 128);
	}
	if (yaml_mapping_has_member(mapping, "shadow-offset-y")) {
		self->shadow_offset_y = CLAMP((gint)yaml_mapping_get_int_member(
			mapping, "shadow-offset-y"), -128, 128);
	}
	/*
	 * What shows through a translucent window.
	 *
	 * Read before the glass keys so an unreadable value leaves the
	 * default standing rather than half-applying a style.  `blur' here
	 * and the older boolean `blur' key are not the same setting: the
	 * boolean switches the blur MODULE's backdrop off entirely, while
	 * this chooses between two modules that both draw one.
	 */
	if (yaml_mapping_has_member(mapping, "window-backdrop")) {
		const gchar *v = yaml_mapping_get_string_member(
			mapping, "window-backdrop");
		GEnumClass  *ec;
		GEnumValue  *ev = NULL;

		if (v != NULL) {
			g_autofree gchar *norm = g_strdup(v);

			ec = (GEnumClass *)g_type_class_ref(
				gowl_backdrop_style_get_type());
			g_strdelimit(norm, "_", '-');
			ev = g_enum_get_value_by_nick(ec, norm);
			g_type_class_unref(ec);
		}
		if (ev != NULL)
			self->backdrop_style = ev->value;
		else
			g_warning("gowl_config: unknown window-backdrop '%s'; "
			          "expected none, blur, glass, water, rain, snow, "
			          "leaves or fizz",
			          v != NULL ? v : "(null)");
	}
	if (yaml_mapping_has_member(mapping, "glass-bevel")) {
		self->glass_bevel = CLAMP(yaml_mapping_get_double_member(
			mapping, "glass-bevel"), 1.0, 400.0);
	}
	if (yaml_mapping_has_member(mapping, "glass-thickness")) {
		self->glass_thickness = CLAMP(yaml_mapping_get_double_member(
			mapping, "glass-thickness"), 0.0, 400.0);
	}
	if (yaml_mapping_has_member(mapping, "glass-slope")) {
		self->glass_slope = CLAMP(yaml_mapping_get_double_member(
			mapping, "glass-slope"), 0.2, 4.0);
	}
	if (yaml_mapping_has_member(mapping, "glass-shape")) {
		const gchar *v = yaml_mapping_get_string_member(
			mapping, "glass-shape");

		if (v != NULL && (g_strcmp0(v, "circle") == 0
		                  || g_strcmp0(v, "squircle") == 0
		                  || g_strcmp0(v, "lip") == 0)) {
			g_free(self->glass_shape);
			self->glass_shape = g_strdup(v);
		} else {
			g_warning("gowl_config: unknown glass-shape '%s'; "
			          "expected circle, squircle or lip",
			          v != NULL ? v : "(null)");
		}
	}
	if (yaml_mapping_has_member(mapping, "glass-dispersion")) {
		self->glass_dispersion = CLAMP(yaml_mapping_get_double_member(
			mapping, "glass-dispersion"), 0.0, 8.0);
	}
	if (yaml_mapping_has_member(mapping, "glass-rim")) {
		self->glass_rim = CLAMP(yaml_mapping_get_double_member(
			mapping, "glass-rim"), 0.0, 4.0);
	}
	if (yaml_mapping_has_member(mapping, "glass-shade")) {
		self->glass_shade = CLAMP(yaml_mapping_get_double_member(
			mapping, "glass-shade"), 0.0, 2.0);
	}
	if (yaml_mapping_has_member(mapping, "glass-edge-width")) {
		self->glass_edge_width = CLAMP(yaml_mapping_get_double_member(
			mapping, "glass-edge-width"), 0.5, 64.0);
	}
	if (yaml_mapping_has_member(mapping, "glass-saturation")) {
		self->glass_saturation = CLAMP(yaml_mapping_get_double_member(
			mapping, "glass-saturation"), 0.0, 3.0);
	}
	if (yaml_mapping_has_member(mapping, "glass-clarity")) {
		self->glass_clarity = CLAMP(yaml_mapping_get_double_member(
			mapping, "glass-clarity"), 0.0, 1.0);
	}
	if (yaml_mapping_has_member(mapping, "glass-centre-clarity")) {
		self->glass_centre_clarity = CLAMP(yaml_mapping_get_double_member(
			mapping, "glass-centre-clarity"), 0.0, 1.0);
	}
	if (yaml_mapping_has_member(mapping, "glass-lens")) {
		self->glass_lens = CLAMP(yaml_mapping_get_double_member(
			mapping, "glass-lens"), 0.0, 0.5);
	}
	if (yaml_mapping_has_member(mapping, "glass-sheen")) {
		self->glass_sheen = CLAMP(yaml_mapping_get_double_member(
			mapping, "glass-sheen"), 0.0, 3.0);
	}
	if (yaml_mapping_has_member(mapping, "glass-light")) {
		self->glass_light = CLAMP(yaml_mapping_get_double_member(
			mapping, "glass-light"), -180.0, 180.0);
	}
	if (yaml_mapping_has_member(mapping, "glass-tint")) {
		const gchar *v = yaml_mapping_get_string_member(mapping, "glass-tint");

		if (v != NULL) {
			g_free(self->glass_tint);
			self->glass_tint = gowl_palette_resolve(self->palette, v);
		}
	}
	if (yaml_mapping_has_member(mapping, "glass-brightness")) {
		self->glass_brightness = CLAMP(yaml_mapping_get_double_member(
			mapping, "glass-brightness"), 0.2, 2.0);
	}
	if (yaml_mapping_has_member(mapping, "glass-opacity")) {
		self->glass_opacity = CLAMP(yaml_mapping_get_double_member(
			mapping, "glass-opacity"), 0.0, 1.0);
	}
	if (yaml_mapping_has_member(mapping, "glass-frost")) {
		self->glass_frost = CLAMP((gint)yaml_mapping_get_int_member(
			mapping, "glass-frost"), 1, 8);
	}
	if (yaml_mapping_has_member(mapping, "glass-frost-passes")) {
		self->glass_frost_passes = CLAMP((gint)yaml_mapping_get_int_member(
			mapping, "glass-frost-passes"), 1, 6);
	}
	if (yaml_mapping_has_member(mapping, "water-preset")) {
		const gchar *v = yaml_mapping_get_string_member(
			mapping, "water-preset");

		if (v != NULL && gowl_config_water_preset_valid(v)) {
			g_free(self->water_preset);
			self->water_preset = g_strdup(v);
		} else {
			g_warning("gowl_config: unknown water-preset '%s'; "
			          "expected pool, fountain, pond, sea or storm",
			          v != NULL ? v : "(null)");
		}
	}
	if (yaml_mapping_has_member(mapping, "water-intensity")) {
		self->water_intensity = CLAMP(yaml_mapping_get_double_member(
			mapping, "water-intensity"), 0.0, 3.0);
	}
	if (yaml_mapping_has_member(mapping, "water-fps")) {
		/* 0 means every frame the output gives us. */
		self->water_fps = CLAMP((gint)yaml_mapping_get_int_member(
			mapping, "water-fps"), 0, 144);
	}
	if (yaml_mapping_has_member(mapping, "water-scale")) {
		self->water_scale = CLAMP((gint)yaml_mapping_get_int_member(
			mapping, "water-scale"), 1, 4);
	}
	if (yaml_mapping_has_member(mapping, "water-tint")) {
		const gchar *v = yaml_mapping_get_string_member(mapping, "water-tint");

		if (v != NULL) {
			g_free(self->water_tint);
			self->water_tint = gowl_palette_resolve(self->palette, v);
		}
	}
	if (yaml_mapping_has_member(mapping, "water-clarity")) {
		self->water_clarity = CLAMP(yaml_mapping_get_double_member(
			mapping, "water-clarity"), 0.0, 1.0);
	}
	if (yaml_mapping_has_member(mapping, "water-opacity")) {
		self->water_opacity = CLAMP(yaml_mapping_get_double_member(
			mapping, "water-opacity"), 0.0, 1.0);
	}
	if (yaml_mapping_has_member(mapping, "water-brightness")) {
		self->water_brightness = CLAMP(yaml_mapping_get_double_member(
			mapping, "water-brightness"), 0.2, 2.0);
	}
	if (yaml_mapping_has_member(mapping, "water-light")) {
		self->water_light = CLAMP(yaml_mapping_get_double_member(
			mapping, "water-light"), -180.0, 180.0);
	}
	if (yaml_mapping_has_member(mapping, "water-frost")) {
		self->water_frost = CLAMP((gint)yaml_mapping_get_int_member(
			mapping, "water-frost"), 1, 8);
	}
	if (yaml_mapping_has_member(mapping, "water-frost-passes")) {
		self->water_frost_passes = CLAMP((gint)yaml_mapping_get_int_member(
			mapping, "water-frost-passes"), 1, 6);
	}
	/* The overrides on the preset.  Each stays at its sentinel until the
	 * config names it, which is how a preset can decide everything it was
	 * not asked about. */
	if (yaml_mapping_has_member(mapping, "water-amplitude")) {
		self->water_amplitude = CLAMP(yaml_mapping_get_double_member(
			mapping, "water-amplitude"), 0.0, 200.0);
	}
	if (yaml_mapping_has_member(mapping, "water-wavelength")) {
		self->water_wavelength = CLAMP(yaml_mapping_get_double_member(
			mapping, "water-wavelength"), 8.0, 2000.0);
	}
	if (yaml_mapping_has_member(mapping, "water-choppiness")) {
		self->water_choppiness = CLAMP(yaml_mapping_get_double_member(
			mapping, "water-choppiness"), 0.0, 1.0);
	}
	if (yaml_mapping_has_member(mapping, "water-depth")) {
		self->water_depth = CLAMP(yaml_mapping_get_double_member(
			mapping, "water-depth"), 0.0, 1000.0);
	}
	if (yaml_mapping_has_member(mapping, "water-drops")) {
		self->water_drops = CLAMP(yaml_mapping_get_double_member(
			mapping, "water-drops"), 0.0, 6.0);
	}
	if (yaml_mapping_has_member(mapping, "water-shore")) {
		self->water_shore = CLAMP(yaml_mapping_get_double_member(
			mapping, "water-shore"), 0.0, 400.0);
	}
	if (yaml_mapping_has_member(mapping, "water-specular")) {
		self->water_specular = CLAMP(yaml_mapping_get_double_member(
			mapping, "water-specular"), 0.0, 3.0);
	}
	if (yaml_mapping_has_member(mapping, "water-caustics")) {
		self->water_caustics = CLAMP(yaml_mapping_get_double_member(
			mapping, "water-caustics"), 0.0, 3.0);
	}
	if (yaml_mapping_has_member(mapping, "water-foam")) {
		self->water_foam = CLAMP(yaml_mapping_get_double_member(
			mapping, "water-foam"), 0.0, 1.0);
	}
	if (yaml_mapping_has_member(mapping, "water-fresnel")) {
		self->water_fresnel = CLAMP(yaml_mapping_get_double_member(
			mapping, "water-fresnel"), 0.0, 1.0);
	}
	if (yaml_mapping_has_member(mapping, "water-speed")) {
		self->water_speed = CLAMP(yaml_mapping_get_double_member(
			mapping, "water-speed"), 0.0, 5.0);
	}
	if (yaml_mapping_has_member(mapping, "rain-preset")) {
		const gchar *v = yaml_mapping_get_string_member(
			mapping, "rain-preset");

		if (v != NULL && gowl_config_rain_preset_valid(v)) {
			g_free(self->rain_preset);
			self->rain_preset = g_strdup(v);
		} else {
			g_warning("gowl_config: unknown rain-preset '%s'; "
			          "expected mist, drizzle, shower, downpour or storm",
			          v != NULL ? v : "(null)");
		}
	}
	if (yaml_mapping_has_member(mapping, "rain-intensity")) {
		self->rain_intensity = CLAMP(yaml_mapping_get_double_member(
			mapping, "rain-intensity"), 0.0, 3.0);
	}
	if (yaml_mapping_has_member(mapping, "rain-fps")) {
		/* 0 means every frame the output gives us. */
		self->rain_fps = CLAMP((gint)yaml_mapping_get_int_member(
			mapping, "rain-fps"), 0, 144);
	}
	if (yaml_mapping_has_member(mapping, "rain-scale")) {
		self->rain_scale = CLAMP((gint)yaml_mapping_get_int_member(
			mapping, "rain-scale"), 1, 4);
	}
	if (yaml_mapping_has_member(mapping, "rain-tint")) {
		const gchar *v = yaml_mapping_get_string_member(mapping, "rain-tint");

		if (v != NULL) {
			g_free(self->rain_tint);
			self->rain_tint = gowl_palette_resolve(self->palette, v);
		}
	}
	if (yaml_mapping_has_member(mapping, "rain-clarity")) {
		self->rain_clarity = CLAMP(yaml_mapping_get_double_member(
			mapping, "rain-clarity"), 0.0, 1.0);
	}
	if (yaml_mapping_has_member(mapping, "rain-opacity")) {
		self->rain_opacity = CLAMP(yaml_mapping_get_double_member(
			mapping, "rain-opacity"), 0.0, 1.0);
	}
	if (yaml_mapping_has_member(mapping, "rain-brightness")) {
		self->rain_brightness = CLAMP(yaml_mapping_get_double_member(
			mapping, "rain-brightness"), 0.2, 2.0);
	}
	if (yaml_mapping_has_member(mapping, "rain-light")) {
		self->rain_light = CLAMP(yaml_mapping_get_double_member(
			mapping, "rain-light"), -180.0, 180.0);
	}
	if (yaml_mapping_has_member(mapping, "rain-frost")) {
		self->rain_frost = CLAMP((gint)yaml_mapping_get_int_member(
			mapping, "rain-frost"), 1, 8);
	}
	if (yaml_mapping_has_member(mapping, "rain-frost-passes")) {
		self->rain_frost_passes = CLAMP((gint)yaml_mapping_get_int_member(
			mapping, "rain-frost-passes"), 1, 6);
	}
	/* The overrides on the preset, each at its sentinel until named. */
	if (yaml_mapping_has_member(mapping, "rain-cell")) {
		self->rain_cell = CLAMP(yaml_mapping_get_double_member(
			mapping, "rain-cell"), 8.0, 400.0);
	}
	if (yaml_mapping_has_member(mapping, "rain-density")) {
		self->rain_density = CLAMP(yaml_mapping_get_double_member(
			mapping, "rain-density"), 0.0, 1.0);
	}
	if (yaml_mapping_has_member(mapping, "rain-bulge")) {
		self->rain_bulge = CLAMP(yaml_mapping_get_double_member(
			mapping, "rain-bulge"), 0.1, 3.0);
	}
	if (yaml_mapping_has_member(mapping, "rain-depth")) {
		self->rain_depth = CLAMP(yaml_mapping_get_double_member(
			mapping, "rain-depth"), 0.0, 20.0);
	}
	if (yaml_mapping_has_member(mapping, "rain-runs")) {
		self->rain_runs = CLAMP(yaml_mapping_get_double_member(
			mapping, "rain-runs"), 0.0, 1.0);
	}
	if (yaml_mapping_has_member(mapping, "rain-run-width")) {
		self->rain_run_width = CLAMP(yaml_mapping_get_double_member(
			mapping, "rain-run-width"), 10.0, 600.0);
	}
	if (yaml_mapping_has_member(mapping, "rain-run-length")) {
		self->rain_run_length = CLAMP(yaml_mapping_get_double_member(
			mapping, "rain-run-length"), 0.0, 4000.0);
	}
	if (yaml_mapping_has_member(mapping, "rain-beads")) {
		self->rain_beads = CLAMP(yaml_mapping_get_double_member(
			mapping, "rain-beads"), 0.0, 1.0);
	}
	if (yaml_mapping_has_member(mapping, "rain-fog")) {
		self->rain_fog = CLAMP(yaml_mapping_get_double_member(
			mapping, "rain-fog"), 0.0, 1.0);
	}
	if (yaml_mapping_has_member(mapping, "rain-specular")) {
		self->rain_specular = CLAMP(yaml_mapping_get_double_member(
			mapping, "rain-specular"), 0.0, 3.0);
	}
	if (yaml_mapping_has_member(mapping, "soap-preset")) {
		const gchar *v = yaml_mapping_get_string_member(mapping,
		                                                "soap-preset");
		if (gowl_config_soap_preset_valid(v)) {
			g_free(self->soap_preset);
			self->soap_preset = g_strdup(v);
		} else if (v != NULL) {
			g_warning("gowl_config: unknown soap-preset '%s'", v);
		}
	}
	if (yaml_mapping_has_member(mapping, "soap-intensity")) {
		self->soap_intensity = CLAMP(yaml_mapping_get_double_member(
			mapping, "soap-intensity"), 0.0, 3.0);
	}
	if (yaml_mapping_has_member(mapping, "soap-fps")) {
		self->soap_fps = CLAMP((gint)yaml_mapping_get_int_member(
			mapping, "soap-fps"), 0, 144);
	}
	if (yaml_mapping_has_member(mapping, "soap-scale")) {
		self->soap_scale = CLAMP((gint)yaml_mapping_get_int_member(
			mapping, "soap-scale"), 1, 4);
	}
	if (yaml_mapping_has_member(mapping, "soap-tint")) {
		const gchar *v = yaml_mapping_get_string_member(mapping, "soap-tint");
		if (v != NULL) {
			g_free(self->soap_tint);
			self->soap_tint = gowl_palette_resolve(self->palette, v);
		}
	}
	if (yaml_mapping_has_member(mapping, "soap-clarity")) {
		self->soap_clarity = CLAMP(yaml_mapping_get_double_member(
			mapping, "soap-clarity"), 0.0, 1.0);
	}
	if (yaml_mapping_has_member(mapping, "soap-opacity")) {
		self->soap_opacity = CLAMP(yaml_mapping_get_double_member(
			mapping, "soap-opacity"), 0.0, 1.0);
	}
	if (yaml_mapping_has_member(mapping, "soap-brightness")) {
		self->soap_brightness = CLAMP(yaml_mapping_get_double_member(
			mapping, "soap-brightness"), 0.0, 2.0);
	}
	if (yaml_mapping_has_member(mapping, "soap-light")) {
		self->soap_light = yaml_mapping_get_double_member(
			mapping, "soap-light");
	}
	if (yaml_mapping_has_member(mapping, "soap-frost")) {
		self->soap_frost = CLAMP((gint)yaml_mapping_get_int_member(
			mapping, "soap-frost"), 1, 8);
	}
	if (yaml_mapping_has_member(mapping, "soap-frost-passes")) {
		self->soap_frost_passes = CLAMP((gint)yaml_mapping_get_int_member(
			mapping, "soap-frost-passes"), 1, 6);
	}
	if (yaml_mapping_has_member(mapping, "soap-thickness")) {
		self->soap_thickness = CLAMP(yaml_mapping_get_double_member(
			mapping, "soap-thickness"), 60.0, 3600.0);
	}
	if (yaml_mapping_has_member(mapping, "soap-thin")) {
		self->soap_thin = CLAMP(yaml_mapping_get_double_member(
			mapping, "soap-thin"), 0.0, 1.0);
	}
	if (yaml_mapping_has_member(mapping, "soap-drain")) {
		self->soap_drain = CLAMP(yaml_mapping_get_double_member(
			mapping, "soap-drain"), 0.0, 6.0);
	}
	if (yaml_mapping_has_member(mapping, "soap-turbulence")) {
		self->soap_turbulence = CLAMP(yaml_mapping_get_double_member(
			mapping, "soap-turbulence"), 0.0, 2.0);
	}
	if (yaml_mapping_has_member(mapping, "soap-swirl")) {
		self->soap_swirl = CLAMP(yaml_mapping_get_double_member(
			mapping, "soap-swirl"), 0.5, 16.0);
	}
	if (yaml_mapping_has_member(mapping, "soap-index")) {
		self->soap_index = CLAMP(yaml_mapping_get_double_member(
			mapping, "soap-index"), 1.05, 2.0);
	}
	if (yaml_mapping_has_member(mapping, "soap-gain")) {
		self->soap_gain = CLAMP(yaml_mapping_get_double_member(
			mapping, "soap-gain"), 0.0, 12.0);
	}
	if (yaml_mapping_has_member(mapping, "soap-sheen")) {
		self->soap_sheen = CLAMP(yaml_mapping_get_double_member(
			mapping, "soap-sheen"), 0.0, 2.0);
	}
	if (yaml_mapping_has_member(mapping, "soap-wedge")) {
		self->soap_wedge = CLAMP(yaml_mapping_get_double_member(
			mapping, "soap-wedge"), 0.0, 2000.0);
	}
	if (yaml_mapping_has_member(mapping, "soap-pop")) {
		self->soap_pop = CLAMP(yaml_mapping_get_double_member(
			mapping, "soap-pop"), 0.0, 0.6);
	}
	if (yaml_mapping_has_member(mapping, "soap-meniscus")) {
		self->soap_meniscus = CLAMP(yaml_mapping_get_double_member(
			mapping, "soap-meniscus"), 0.0, 2.0);
	}
	if (yaml_mapping_has_member(mapping, "soap-fog")) {
		self->soap_fog = CLAMP(yaml_mapping_get_double_member(
			mapping, "soap-fog"), 0.0, 1.0);
	}
	if (yaml_mapping_has_member(mapping, "soap-speed")) {
		self->soap_speed = CLAMP(yaml_mapping_get_double_member(
			mapping, "soap-speed"), 0.0, 5.0);
	}
	if (yaml_mapping_has_member(mapping, "soap-life")) {
		self->soap_life = CLAMP(yaml_mapping_get_double_member(
			mapping, "soap-life"), 1.0, 600.0);
	}

	if (yaml_mapping_has_member(mapping, "embers-preset")) {
		const gchar *v = yaml_mapping_get_string_member(mapping,
		                                                "embers-preset");
		if (gowl_config_embers_preset_valid(v)) {
			g_free(self->embers_preset);
			self->embers_preset = g_strdup(v);
		} else if (v != NULL) {
			g_warning("gowl_config: unknown embers-preset '%s'", v);
		}
	}
	if (yaml_mapping_has_member(mapping, "embers-intensity")) {
		self->embers_intensity = CLAMP(yaml_mapping_get_double_member(
			mapping, "embers-intensity"), 0.0, 3.0);
	}
	if (yaml_mapping_has_member(mapping, "embers-fps")) {
		self->embers_fps = CLAMP((gint)yaml_mapping_get_int_member(
			mapping, "embers-fps"), 0, 144);
	}
	if (yaml_mapping_has_member(mapping, "embers-scale")) {
		self->embers_scale = CLAMP((gint)yaml_mapping_get_int_member(
			mapping, "embers-scale"), 1, 4);
	}
	if (yaml_mapping_has_member(mapping, "embers-tint")) {
		const gchar *v = yaml_mapping_get_string_member(mapping, "embers-tint");
		if (v != NULL) {
			g_free(self->embers_tint);
			self->embers_tint = gowl_palette_resolve(self->palette, v);
		}
	}
	if (yaml_mapping_has_member(mapping, "embers-clarity")) {
		self->embers_clarity = CLAMP(yaml_mapping_get_double_member(
			mapping, "embers-clarity"), 0.0, 1.0);
	}
	if (yaml_mapping_has_member(mapping, "embers-opacity")) {
		self->embers_opacity = CLAMP(yaml_mapping_get_double_member(
			mapping, "embers-opacity"), 0.0, 1.0);
	}
	if (yaml_mapping_has_member(mapping, "embers-brightness")) {
		self->embers_brightness = CLAMP(yaml_mapping_get_double_member(
			mapping, "embers-brightness"), 0.0, 2.0);
	}
	if (yaml_mapping_has_member(mapping, "embers-frost")) {
		self->embers_frost = CLAMP((gint)yaml_mapping_get_int_member(
			mapping, "embers-frost"), 1, 8);
	}
	if (yaml_mapping_has_member(mapping, "embers-frost-passes")) {
		self->embers_frost_passes = CLAMP((gint)yaml_mapping_get_int_member(
			mapping, "embers-frost-passes"), 1, 6);
	}
	if (yaml_mapping_has_member(mapping, "embers-column")) {
		self->embers_column = CLAMP(yaml_mapping_get_double_member(
			mapping, "embers-column"), 16.0, 600.0);
	}
	if (yaml_mapping_has_member(mapping, "embers-density")) {
		self->embers_density = CLAMP(yaml_mapping_get_double_member(
			mapping, "embers-density"), 0.0, 1.0);
	}
	if (yaml_mapping_has_member(mapping, "embers-ember")) {
		self->embers_ember = CLAMP(yaml_mapping_get_double_member(
			mapping, "embers-ember"), 0.4, 40.0);
	}
	if (yaml_mapping_has_member(mapping, "embers-spacing")) {
		self->embers_spacing = CLAMP(yaml_mapping_get_double_member(
			mapping, "embers-spacing"), 0.0, 1.0);
	}
	if (yaml_mapping_has_member(mapping, "embers-sway")) {
		self->embers_sway = CLAMP(yaml_mapping_get_double_member(
			mapping, "embers-sway"), 0.0, 200.0);
	}
	if (yaml_mapping_has_member(mapping, "embers-drag")) {
		self->embers_drag = CLAMP(yaml_mapping_get_double_member(
			mapping, "embers-drag"), 0.05, 8.0);
	}
	if (yaml_mapping_has_member(mapping, "embers-temperature")) {
		self->embers_temperature = CLAMP(yaml_mapping_get_double_member(
			mapping, "embers-temperature"), 900.0, 4000.0);
	}
	if (yaml_mapping_has_member(mapping, "embers-cool")) {
		self->embers_cool = CLAMP(yaml_mapping_get_double_member(
			mapping, "embers-cool"), 0.0, 6.0);
	}
	if (yaml_mapping_has_member(mapping, "embers-flicker")) {
		self->embers_flicker = CLAMP(yaml_mapping_get_double_member(
			mapping, "embers-flicker"), 0.0, 2.0);
	}
	if (yaml_mapping_has_member(mapping, "embers-ash")) {
		self->embers_ash = CLAMP(yaml_mapping_get_double_member(
			mapping, "embers-ash"), 0.0, 0.8);
	}
	if (yaml_mapping_has_member(mapping, "embers-glow")) {
		self->embers_glow = CLAMP(yaml_mapping_get_double_member(
			mapping, "embers-glow"), 0.0, 3.0);
	}
	if (yaml_mapping_has_member(mapping, "embers-hearth")) {
		self->embers_hearth = CLAMP(yaml_mapping_get_double_member(
			mapping, "embers-hearth"), 0.0, 2.0);
	}
	if (yaml_mapping_has_member(mapping, "embers-haze")) {
		self->embers_haze = CLAMP(yaml_mapping_get_double_member(
			mapping, "embers-haze"), 0.0, 60.0);
	}
	if (yaml_mapping_has_member(mapping, "embers-haze-scale")) {
		self->embers_haze_scale = CLAMP(yaml_mapping_get_double_member(
			mapping, "embers-haze-scale"), 0.5, 24.0);
	}
	if (yaml_mapping_has_member(mapping, "embers-fog")) {
		self->embers_fog = CLAMP(yaml_mapping_get_double_member(
			mapping, "embers-fog"), 0.0, 1.0);
	}
	if (yaml_mapping_has_member(mapping, "embers-speed")) {
		self->embers_speed = CLAMP(yaml_mapping_get_double_member(
			mapping, "embers-speed"), 0.0, 5.0);
	}

	if (yaml_mapping_has_member(mapping, "submerged-preset")) {
		const gchar *v = yaml_mapping_get_string_member(mapping,
		                                                "submerged-preset");
		if (gowl_config_submerged_preset_valid(v)) {
			g_free(self->submerged_preset);
			self->submerged_preset = g_strdup(v);
		} else if (v != NULL) {
			g_warning("gowl_config: unknown submerged-preset '%s'", v);
		}
	}
	if (yaml_mapping_has_member(mapping, "submerged-intensity")) {
		self->submerged_intensity = CLAMP(yaml_mapping_get_double_member(
			mapping, "submerged-intensity"), 0.0, 3.0);
	}
	if (yaml_mapping_has_member(mapping, "submerged-fps")) {
		self->submerged_fps = CLAMP((gint)yaml_mapping_get_int_member(
			mapping, "submerged-fps"), 0, 144);
	}
	if (yaml_mapping_has_member(mapping, "submerged-scale")) {
		self->submerged_scale = CLAMP((gint)yaml_mapping_get_int_member(
			mapping, "submerged-scale"), 1, 4);
	}
	if (yaml_mapping_has_member(mapping, "submerged-water")) {
		const gchar *v = yaml_mapping_get_string_member(mapping, "submerged-water");
		if (v != NULL) {
			g_free(self->submerged_water);
			self->submerged_water = gowl_palette_resolve(self->palette, v);
		}
	}
	if (yaml_mapping_has_member(mapping, "submerged-clarity")) {
		self->submerged_clarity = CLAMP(yaml_mapping_get_double_member(
			mapping, "submerged-clarity"), 0.0, 1.0);
	}
	if (yaml_mapping_has_member(mapping, "submerged-opacity")) {
		self->submerged_opacity = CLAMP(yaml_mapping_get_double_member(
			mapping, "submerged-opacity"), 0.0, 1.0);
	}
	if (yaml_mapping_has_member(mapping, "submerged-brightness")) {
		self->submerged_brightness = CLAMP(yaml_mapping_get_double_member(
			mapping, "submerged-brightness"), 0.0, 2.0);
	}
	if (yaml_mapping_has_member(mapping, "submerged-frost")) {
		self->submerged_frost = CLAMP((gint)yaml_mapping_get_int_member(
			mapping, "submerged-frost"), 1, 8);
	}
	if (yaml_mapping_has_member(mapping, "submerged-frost-passes")) {
		self->submerged_frost_passes = CLAMP((gint)yaml_mapping_get_int_member(
			mapping, "submerged-frost-passes"), 1, 6);
	}
	if (yaml_mapping_has_member(mapping, "submerged-depth")) {
		self->submerged_depth = CLAMP(yaml_mapping_get_double_member(
			mapping, "submerged-depth"), 0.0, 30.0);
	}
	if (yaml_mapping_has_member(mapping, "submerged-extinction-r")) {
		self->submerged_extinction_r = CLAMP(yaml_mapping_get_double_member(
			mapping, "submerged-extinction-r"), 0.0, 4.0);
	}
	if (yaml_mapping_has_member(mapping, "submerged-extinction-g")) {
		self->submerged_extinction_g = CLAMP(yaml_mapping_get_double_member(
			mapping, "submerged-extinction-g"), 0.0, 4.0);
	}
	if (yaml_mapping_has_member(mapping, "submerged-extinction-b")) {
		self->submerged_extinction_b = CLAMP(yaml_mapping_get_double_member(
			mapping, "submerged-extinction-b"), 0.0, 4.0);
	}
	if (yaml_mapping_has_member(mapping, "submerged-murk")) {
		self->submerged_murk = CLAMP(yaml_mapping_get_double_member(
			mapping, "submerged-murk"), 0.0, 2.0);
	}
	if (yaml_mapping_has_member(mapping, "submerged-caustics")) {
		self->submerged_caustics = CLAMP(yaml_mapping_get_double_member(
			mapping, "submerged-caustics"), 0.0, 3.0);
	}
	if (yaml_mapping_has_member(mapping, "submerged-caustic-scale")) {
		self->submerged_caustic_scale = CLAMP(yaml_mapping_get_double_member(
			mapping, "submerged-caustic-scale"), 0.5, 24.0);
	}
	if (yaml_mapping_has_member(mapping, "submerged-shafts")) {
		self->submerged_shafts = CLAMP(yaml_mapping_get_double_member(
			mapping, "submerged-shafts"), 0.0, 3.0);
	}
	if (yaml_mapping_has_member(mapping, "submerged-shaft-lean")) {
		self->submerged_shaft_lean = CLAMP(yaml_mapping_get_double_member(
			mapping, "submerged-shaft-lean"), -2.0, 2.0);
	}
	if (yaml_mapping_has_member(mapping, "submerged-motes")) {
		self->submerged_motes = CLAMP(yaml_mapping_get_double_member(
			mapping, "submerged-motes"), 0.0, 1.0);
	}
	if (yaml_mapping_has_member(mapping, "submerged-mote-size")) {
		self->submerged_mote_size = CLAMP(yaml_mapping_get_double_member(
			mapping, "submerged-mote-size"), 0.4, 24.0);
	}
	if (yaml_mapping_has_member(mapping, "submerged-surface")) {
		self->submerged_surface = CLAMP(yaml_mapping_get_double_member(
			mapping, "submerged-surface"), 0.0, 1.0);
	}
	if (yaml_mapping_has_member(mapping, "submerged-sway")) {
		self->submerged_sway = CLAMP(yaml_mapping_get_double_member(
			mapping, "submerged-sway"), 0.0, 80.0);
	}
	if (yaml_mapping_has_member(mapping, "submerged-fog")) {
		self->submerged_fog = CLAMP(yaml_mapping_get_double_member(
			mapping, "submerged-fog"), 0.0, 1.0);
	}
	if (yaml_mapping_has_member(mapping, "submerged-speed")) {
		self->submerged_speed = CLAMP(yaml_mapping_get_double_member(
			mapping, "submerged-speed"), 0.0, 5.0);
	}
	if (yaml_mapping_has_member(mapping, "submerged-drift")) {
		self->submerged_drift = CLAMP(yaml_mapping_get_double_member(
			mapping, "submerged-drift"), 1.0, 600.0);
	}

	if (yaml_mapping_has_member(mapping, "dew-preset")) {
		const gchar *v = yaml_mapping_get_string_member(mapping,
		                                                "dew-preset");
		if (gowl_config_dew_preset_valid(v)) {
			g_free(self->dew_preset);
			self->dew_preset = g_strdup(v);
		} else if (v != NULL) {
			g_warning("gowl_config: unknown dew-preset '%s'", v);
		}
	}
	if (yaml_mapping_has_member(mapping, "dew-intensity")) {
		self->dew_intensity = CLAMP(yaml_mapping_get_double_member(
			mapping, "dew-intensity"), 0.0, 3.0);
	}
	if (yaml_mapping_has_member(mapping, "dew-fps")) {
		self->dew_fps = CLAMP((gint)yaml_mapping_get_int_member(
			mapping, "dew-fps"), 0, 144);
	}
	if (yaml_mapping_has_member(mapping, "dew-scale")) {
		self->dew_scale = CLAMP((gint)yaml_mapping_get_int_member(
			mapping, "dew-scale"), 1, 4);
	}
	if (yaml_mapping_has_member(mapping, "dew-tint")) {
		const gchar *v = yaml_mapping_get_string_member(mapping, "dew-tint");
		if (v != NULL) {
			g_free(self->dew_tint);
			self->dew_tint = gowl_palette_resolve(self->palette, v);
		}
	}
	if (yaml_mapping_has_member(mapping, "dew-clarity")) {
		self->dew_clarity = CLAMP(yaml_mapping_get_double_member(
			mapping, "dew-clarity"), 0.0, 1.0);
	}
	if (yaml_mapping_has_member(mapping, "dew-opacity")) {
		self->dew_opacity = CLAMP(yaml_mapping_get_double_member(
			mapping, "dew-opacity"), 0.0, 1.0);
	}
	if (yaml_mapping_has_member(mapping, "dew-brightness")) {
		self->dew_brightness = CLAMP(yaml_mapping_get_double_member(
			mapping, "dew-brightness"), 0.0, 2.0);
	}
	if (yaml_mapping_has_member(mapping, "dew-light")) {
		self->dew_light = yaml_mapping_get_double_member(
			mapping, "dew-light");
	}
	if (yaml_mapping_has_member(mapping, "dew-frost")) {
		self->dew_frost = CLAMP((gint)yaml_mapping_get_int_member(
			mapping, "dew-frost"), 1, 8);
	}
	if (yaml_mapping_has_member(mapping, "dew-frost-passes")) {
		self->dew_frost_passes = CLAMP((gint)yaml_mapping_get_int_member(
			mapping, "dew-frost-passes"), 1, 6);
	}
	if (yaml_mapping_has_member(mapping, "dew-radials")) {
		self->dew_radials = CLAMP(yaml_mapping_get_double_member(
			mapping, "dew-radials"), 3.0, 48.0);
	}
	if (yaml_mapping_has_member(mapping, "dew-pitch")) {
		self->dew_pitch = CLAMP(yaml_mapping_get_double_member(
			mapping, "dew-pitch"), 16.0, 600.0);
	}
	if (yaml_mapping_has_member(mapping, "dew-thread")) {
		self->dew_thread = CLAMP(yaml_mapping_get_double_member(
			mapping, "dew-thread"), 0.4, 12.0);
	}
	if (yaml_mapping_has_member(mapping, "dew-drop")) {
		self->dew_drop = CLAMP(yaml_mapping_get_double_member(
			mapping, "dew-drop"), 0.5, 60.0);
	}
	if (yaml_mapping_has_member(mapping, "dew-spacing")) {
		self->dew_spacing = CLAMP(yaml_mapping_get_double_member(
			mapping, "dew-spacing"), 8.0, 400.0);
	}
	if (yaml_mapping_has_member(mapping, "dew-sag")) {
		self->dew_sag = CLAMP(yaml_mapping_get_double_member(
			mapping, "dew-sag"), 0.0, 200.0);
	}
	if (yaml_mapping_has_member(mapping, "dew-depth")) {
		self->dew_depth = CLAMP(yaml_mapping_get_double_member(
			mapping, "dew-depth"), 0.0, 20.0);
	}
	if (yaml_mapping_has_member(mapping, "dew-bulge")) {
		self->dew_bulge = CLAMP(yaml_mapping_get_double_member(
			mapping, "dew-bulge"), 0.0, 3.0);
	}
	if (yaml_mapping_has_member(mapping, "dew-silk")) {
		self->dew_silk = CLAMP(yaml_mapping_get_double_member(
			mapping, "dew-silk"), 0.0, 3.0);
	}
	if (yaml_mapping_has_member(mapping, "dew-glint")) {
		self->dew_glint = CLAMP(yaml_mapping_get_double_member(
			mapping, "dew-glint"), 0.0, 4.0);
	}
	if (yaml_mapping_has_member(mapping, "dew-shine")) {
		self->dew_shine = CLAMP(yaml_mapping_get_double_member(
			mapping, "dew-shine"), 1.0, 256.0);
	}
	if (yaml_mapping_has_member(mapping, "dew-rim")) {
		self->dew_rim = CLAMP(yaml_mapping_get_double_member(
			mapping, "dew-rim"), 0.0, 1.0);
	}
	if (yaml_mapping_has_member(mapping, "dew-sway")) {
		self->dew_sway = CLAMP(yaml_mapping_get_double_member(
			mapping, "dew-sway"), 0.0, 40.0);
	}
	if (yaml_mapping_has_member(mapping, "dew-fog")) {
		self->dew_fog = CLAMP(yaml_mapping_get_double_member(
			mapping, "dew-fog"), 0.0, 1.0);
	}
	if (yaml_mapping_has_member(mapping, "dew-speed")) {
		self->dew_speed = CLAMP(yaml_mapping_get_double_member(
			mapping, "dew-speed"), 0.0, 5.0);
	}

	if (yaml_mapping_has_member(mapping, "bokeh-radius")) {
		self->bokeh_radius = CLAMP(yaml_mapping_get_double_member(
			mapping, "bokeh-radius"), 0.0, 200.0);
	}
	if (yaml_mapping_has_member(mapping, "bokeh-downscale")) {
		self->bokeh_downscale = CLAMP((gint)yaml_mapping_get_int_member(
			mapping, "bokeh-downscale"), 1, 4);
	}
	if (yaml_mapping_has_member(mapping, "bokeh-samples")) {
		self->bokeh_samples = CLAMP((gint)yaml_mapping_get_int_member(
			mapping, "bokeh-samples"), 8, 128);
	}
	if (yaml_mapping_has_member(mapping, "bokeh-blades")) {
		/* Under 3 is a circle: a lens wide open has no straight edges
		 * to show.  12 is past where anybody could tell. */
		self->bokeh_blades = CLAMP((gint)yaml_mapping_get_int_member(
			mapping, "bokeh-blades"), 0, 12);
	}
	if (yaml_mapping_has_member(mapping, "bokeh-rotation")) {
		self->bokeh_rotation = yaml_mapping_get_double_member(
			mapping, "bokeh-rotation");
	}
	if (yaml_mapping_has_member(mapping, "bokeh-highlight")) {
		self->bokeh_highlight = CLAMP(yaml_mapping_get_double_member(
			mapping, "bokeh-highlight"), 0.0, 32.0);
	}
	if (yaml_mapping_has_member(mapping, "bokeh-threshold")) {
		self->bokeh_threshold = CLAMP(yaml_mapping_get_double_member(
			mapping, "bokeh-threshold"), 0.0, 0.99);
	}
	if (yaml_mapping_has_member(mapping, "bokeh-edge")) {
		self->bokeh_edge = CLAMP(yaml_mapping_get_double_member(
			mapping, "bokeh-edge"), 0.0, 2.0);
	}
	if (yaml_mapping_has_member(mapping, "bokeh-brightness")) {
		self->bokeh_brightness = CLAMP(yaml_mapping_get_double_member(
			mapping, "bokeh-brightness"), 0.0, 2.0);
	}
	if (yaml_mapping_has_member(mapping, "rain-lightning")) {
		self->rain_lightning = yaml_mapping_get_boolean_member(
			mapping, "rain-lightning");
	}
	if (yaml_mapping_has_member(mapping, "rain-lightning-rate")) {
		/* Half a second is a strobe and five minutes is a rumour; both
		 * ends are reachable and neither is a default. */
		self->rain_lightning_rate = CLAMP(yaml_mapping_get_double_member(
			mapping, "rain-lightning-rate"), 0.5, 300.0);
	}
	if (yaml_mapping_has_member(mapping, "rain-lightning-power")) {
		self->rain_lightning_power = CLAMP(yaml_mapping_get_double_member(
			mapping, "rain-lightning-power"), 0.0, 2.0);
	}
	if (yaml_mapping_has_member(mapping, "rain-impact")) {
		self->rain_impact = CLAMP(yaml_mapping_get_double_member(
			mapping, "rain-impact"), 0.0, 1.0);
	}
	if (yaml_mapping_has_member(mapping, "rain-speed")) {
		self->rain_speed = CLAMP(yaml_mapping_get_double_member(
			mapping, "rain-speed"), 0.0, 5.0);
	}
	if (yaml_mapping_has_member(mapping, "fizz-preset")) {
		const gchar *v = yaml_mapping_get_string_member(
			mapping, "fizz-preset");

		if (v != NULL && gowl_config_fizz_preset_valid(v)) {
			g_free(self->fizz_preset);
			self->fizz_preset = g_strdup(v);
		} else {
			g_warning("gowl_config: unknown fizz-preset '%s'; "
			          "expected flat, sparkling, soda, seltzer or champagne",
			          v != NULL ? v : "(null)");
		}
	}
	if (yaml_mapping_has_member(mapping, "fizz-intensity")) {
		self->fizz_intensity = CLAMP(yaml_mapping_get_double_member(
			mapping, "fizz-intensity"), 0.0, 3.0);
	}
	/* 0 means every frame the output gives us. */
	if (yaml_mapping_has_member(mapping, "fizz-fps")) {
		self->fizz_fps = CLAMP((gint)yaml_mapping_get_int_member(
			mapping, "fizz-fps"), 0, 144);
	}
	if (yaml_mapping_has_member(mapping, "fizz-scale")) {
		self->fizz_scale = CLAMP((gint)yaml_mapping_get_int_member(
			mapping, "fizz-scale"), 1, 4);
	}
	if (yaml_mapping_has_member(mapping, "fizz-tint")) {
		const gchar *v = yaml_mapping_get_string_member(mapping, "fizz-tint");

		if (v != NULL) {
			g_free(self->fizz_tint);
			self->fizz_tint = gowl_palette_resolve(self->palette, v);
		}
	}
	if (yaml_mapping_has_member(mapping, "fizz-clarity")) {
		self->fizz_clarity = CLAMP(yaml_mapping_get_double_member(
			mapping, "fizz-clarity"), 0.0, 1.0);
	}
	if (yaml_mapping_has_member(mapping, "fizz-opacity")) {
		self->fizz_opacity = CLAMP(yaml_mapping_get_double_member(
			mapping, "fizz-opacity"), 0.0, 1.0);
	}
	if (yaml_mapping_has_member(mapping, "fizz-brightness")) {
		self->fizz_brightness = CLAMP(yaml_mapping_get_double_member(
			mapping, "fizz-brightness"), 0.2, 2.0);
	}
	if (yaml_mapping_has_member(mapping, "fizz-light")) {
		self->fizz_light = CLAMP(yaml_mapping_get_double_member(
			mapping, "fizz-light"), -180.0, 180.0);
	}
	if (yaml_mapping_has_member(mapping, "fizz-frost")) {
		self->fizz_frost = CLAMP((gint)yaml_mapping_get_int_member(
			mapping, "fizz-frost"), 1, 8);
	}
	if (yaml_mapping_has_member(mapping, "fizz-frost-passes")) {
		self->fizz_frost_passes = CLAMP((gint)yaml_mapping_get_int_member(
			mapping, "fizz-frost-passes"), 1, 6);
	}
	/* The overrides on the preset, each at its sentinel until named. */
	if (yaml_mapping_has_member(mapping, "fizz-cell")) {
		self->fizz_cell = CLAMP(yaml_mapping_get_double_member(
			mapping, "fizz-cell"), 8.0, 400.0);
	}
	if (yaml_mapping_has_member(mapping, "fizz-bubble")) {
		self->fizz_bubble = CLAMP(yaml_mapping_get_double_member(
			mapping, "fizz-bubble"), 0.01, 0.22);
	}
	if (yaml_mapping_has_member(mapping, "fizz-growth")) {
		self->fizz_growth = CLAMP(yaml_mapping_get_double_member(
			mapping, "fizz-growth"), 0.0, 3.0);
	}
	if (yaml_mapping_has_member(mapping, "fizz-sites")) {
		self->fizz_sites = CLAMP(yaml_mapping_get_double_member(
			mapping, "fizz-sites"), 0.0, 1.0);
	}
	if (yaml_mapping_has_member(mapping, "fizz-site-width")) {
		self->fizz_site_width = CLAMP(yaml_mapping_get_double_member(
			mapping, "fizz-site-width"), 12.0, 600.0);
	}
	if (yaml_mapping_has_member(mapping, "fizz-spacing")) {
		self->fizz_spacing = CLAMP(yaml_mapping_get_double_member(
			mapping, "fizz-spacing"), 0.0, 1.0);
	}
	if (yaml_mapping_has_member(mapping, "fizz-stray")) {
		self->fizz_stray = CLAMP(yaml_mapping_get_double_member(
			mapping, "fizz-stray"), 0.0, 1.0);
	}
	if (yaml_mapping_has_member(mapping, "fizz-cling")) {
		self->fizz_cling = CLAMP(yaml_mapping_get_double_member(
			mapping, "fizz-cling"), 0.0, 1.0);
	}
	if (yaml_mapping_has_member(mapping, "fizz-wobble")) {
		self->fizz_wobble = CLAMP(yaml_mapping_get_double_member(
			mapping, "fizz-wobble"), 0.0, 200.0);
	}
	if (yaml_mapping_has_member(mapping, "fizz-foam")) {
		self->fizz_foam = CLAMP(yaml_mapping_get_double_member(
			mapping, "fizz-foam"), 0.0, 1.0);
	}
	if (yaml_mapping_has_member(mapping, "fizz-foam-depth")) {
		self->fizz_foam_depth = CLAMP(yaml_mapping_get_double_member(
			mapping, "fizz-foam-depth"), 0.0, 2000.0);
	}
	if (yaml_mapping_has_member(mapping, "fizz-depth")) {
		self->fizz_depth = CLAMP(yaml_mapping_get_double_member(
			mapping, "fizz-depth"), 0.0, 20.0);
	}
	if (yaml_mapping_has_member(mapping, "fizz-mirror")) {
		self->fizz_mirror = CLAMP(yaml_mapping_get_double_member(
			mapping, "fizz-mirror"), 0.0, 1.0);
	}
	if (yaml_mapping_has_member(mapping, "fizz-fog")) {
		self->fizz_fog = CLAMP(yaml_mapping_get_double_member(
			mapping, "fizz-fog"), 0.0, 1.0);
	}
	if (yaml_mapping_has_member(mapping, "fizz-specular")) {
		self->fizz_specular = CLAMP(yaml_mapping_get_double_member(
			mapping, "fizz-specular"), 0.0, 3.0);
	}
	if (yaml_mapping_has_member(mapping, "fizz-speed")) {
		self->fizz_speed = CLAMP(yaml_mapping_get_double_member(
			mapping, "fizz-speed"), 0.0, 5.0);
	}
	if (yaml_mapping_has_member(mapping, "leaves-preset")) {
		const gchar *v = yaml_mapping_get_string_member(
			mapping, "leaves-preset");

		if (v != NULL && gowl_config_leaves_preset_valid(v)) {
			g_free(self->leaves_preset);
			self->leaves_preset = g_strdup(v);
		} else {
			g_warning("gowl_config: unknown leaves-preset '%s'; "
			          "expected turning, autumn, peak, blustery or gale",
			          v != NULL ? v : "(null)");
		}
	}
	if (yaml_mapping_has_member(mapping, "leaves-intensity")) {
		self->leaves_intensity = CLAMP(yaml_mapping_get_double_member(
			mapping, "leaves-intensity"), 0.0, 3.0);
	}
	if (yaml_mapping_has_member(mapping, "leaves-fps")) {
		self->leaves_fps = CLAMP((gint)yaml_mapping_get_int_member(
			mapping, "leaves-fps"), 0, 144);
	}
	if (yaml_mapping_has_member(mapping, "leaves-scale")) {
		self->leaves_scale = CLAMP((gint)yaml_mapping_get_int_member(
			mapping, "leaves-scale"), 1, 4);
	}
	if (yaml_mapping_has_member(mapping, "leaves-warm")) {
		const gchar *v = yaml_mapping_get_string_member(mapping, "leaves-warm");

		if (v != NULL) {
			g_free(self->leaves_warm);
			self->leaves_warm = gowl_palette_resolve(self->palette, v);
		}
	}
	if (yaml_mapping_has_member(mapping, "leaves-gold")) {
		const gchar *v = yaml_mapping_get_string_member(mapping, "leaves-gold");

		if (v != NULL) {
			g_free(self->leaves_gold);
			self->leaves_gold = gowl_palette_resolve(self->palette, v);
		}
	}
	if (yaml_mapping_has_member(mapping, "leaves-dry")) {
		const gchar *v = yaml_mapping_get_string_member(mapping, "leaves-dry");

		if (v != NULL) {
			g_free(self->leaves_dry);
			self->leaves_dry = gowl_palette_resolve(self->palette, v);
		}
	}
	if (yaml_mapping_has_member(mapping, "leaves-opacity")) {
		self->leaves_opacity = CLAMP(yaml_mapping_get_double_member(
			mapping, "leaves-opacity"), 0.0, 1.0);
	}
	if (yaml_mapping_has_member(mapping, "leaves-brightness")) {
		self->leaves_brightness = CLAMP(yaml_mapping_get_double_member(
			mapping, "leaves-brightness"), 0.2, 2.0);
	}
	if (yaml_mapping_has_member(mapping, "leaves-light")) {
		self->leaves_light = CLAMP(yaml_mapping_get_double_member(
			mapping, "leaves-light"), -180.0, 180.0);
	}
	if (yaml_mapping_has_member(mapping, "leaves-frost")) {
		self->leaves_frost = CLAMP((gint)yaml_mapping_get_int_member(
			mapping, "leaves-frost"), 1, 8);
	}
	if (yaml_mapping_has_member(mapping, "leaves-frost-passes")) {
		self->leaves_frost_passes = CLAMP((gint)yaml_mapping_get_int_member(
			mapping, "leaves-frost-passes"), 1, 6);
	}
	/* The overrides on the preset, each at its sentinel until named. */
	if (yaml_mapping_has_member(mapping, "leaves-leaf")) {
		self->leaves_leaf = CLAMP(yaml_mapping_get_double_member(
			mapping, "leaves-leaf"), 8.0, 500.0);
	}
	if (yaml_mapping_has_member(mapping, "leaves-cell")) {
		self->leaves_cell = CLAMP(yaml_mapping_get_double_member(
			mapping, "leaves-cell"), 32.0, 2000.0);
	}
	if (yaml_mapping_has_member(mapping, "leaves-stuck")) {
		self->leaves_stuck = CLAMP(yaml_mapping_get_double_member(
			mapping, "leaves-stuck"), 0.0, 1.0);
	}
	if (yaml_mapping_has_member(mapping, "leaves-column")) {
		self->leaves_column = CLAMP(yaml_mapping_get_double_member(
			mapping, "leaves-column"), 32.0, 2000.0);
	}
	if (yaml_mapping_has_member(mapping, "leaves-falling")) {
		self->leaves_falling = CLAMP(yaml_mapping_get_double_member(
			mapping, "leaves-falling"), 0.0, 1.0);
	}
	if (yaml_mapping_has_member(mapping, "leaves-flutter")) {
		self->leaves_flutter = CLAMP(yaml_mapping_get_double_member(
			mapping, "leaves-flutter"), 0.0, 0.45);
	}
	if (yaml_mapping_has_member(mapping, "leaves-tumble")) {
		self->leaves_tumble = CLAMP(yaml_mapping_get_double_member(
			mapping, "leaves-tumble"), 0.0, 12.0);
	}
	if (yaml_mapping_has_member(mapping, "leaves-wind")) {
		self->leaves_wind = CLAMP(yaml_mapping_get_double_member(
			mapping, "leaves-wind"), -2000.0, 2000.0);
	}
	if (yaml_mapping_has_member(mapping, "leaves-gust")) {
		self->leaves_gust = CLAMP(yaml_mapping_get_double_member(
			mapping, "leaves-gust"), 0.0, 3000.0);
	}
	if (yaml_mapping_has_member(mapping, "leaves-gustiness")) {
		self->leaves_gustiness = CLAMP(yaml_mapping_get_double_member(
			mapping, "leaves-gustiness"), 0.0, 2.0);
	}
	if (yaml_mapping_has_member(mapping, "leaves-curl")) {
		self->leaves_curl = CLAMP(yaml_mapping_get_double_member(
			mapping, "leaves-curl"), 0.0, 1.0);
	}
	if (yaml_mapping_has_member(mapping, "leaves-veins")) {
		self->leaves_veins = CLAMP(yaml_mapping_get_double_member(
			mapping, "leaves-veins"), 0.0, 1.0);
	}
	if (yaml_mapping_has_member(mapping, "leaves-translucency")) {
		self->leaves_translucency = CLAMP(yaml_mapping_get_double_member(
			mapping, "leaves-translucency"), 0.0, 1.0);
	}
	if (yaml_mapping_has_member(mapping, "leaves-gloss")) {
		self->leaves_gloss = CLAMP(yaml_mapping_get_double_member(
			mapping, "leaves-gloss"), 0.0, 2.0);
	}
	if (yaml_mapping_has_member(mapping, "leaves-shadow")) {
		self->leaves_shadow = CLAMP(yaml_mapping_get_double_member(
			mapping, "leaves-shadow"), 0.0, 1.0);
	}
	if (yaml_mapping_has_member(mapping, "leaves-fog")) {
		self->leaves_fog = CLAMP(yaml_mapping_get_double_member(
			mapping, "leaves-fog"), 0.0, 1.0);
	}
	if (yaml_mapping_has_member(mapping, "leaves-speed")) {
		self->leaves_speed = CLAMP(yaml_mapping_get_double_member(
			mapping, "leaves-speed"), 0.0, 5.0);
	}
	if (yaml_mapping_has_member(mapping, "leaves-tenure")) {
		self->leaves_tenure = CLAMP(yaml_mapping_get_double_member(
			mapping, "leaves-tenure"), 1.0, 600.0);
	}
	if (yaml_mapping_has_member(mapping, "snow-preset")) {
		const gchar *v = yaml_mapping_get_string_member(
			mapping, "snow-preset");

		if (v != NULL && gowl_config_snow_preset_valid(v)) {
			g_free(self->snow_preset);
			self->snow_preset = g_strdup(v);
		} else {
			g_warning("gowl_config: unknown snow-preset '%s'; "
			          "expected flurry, light, steady, heavy or blizzard",
			          v != NULL ? v : "(null)");
		}
	}
	if (yaml_mapping_has_member(mapping, "snow-intensity")) {
		self->snow_intensity = CLAMP(yaml_mapping_get_double_member(
			mapping, "snow-intensity"), 0.0, 3.0);
	}
	if (yaml_mapping_has_member(mapping, "snow-fps")) {
		self->snow_fps = CLAMP((gint)yaml_mapping_get_int_member(
			mapping, "snow-fps"), 0, 144);
	}
	if (yaml_mapping_has_member(mapping, "snow-scale")) {
		self->snow_scale = CLAMP((gint)yaml_mapping_get_int_member(
			mapping, "snow-scale"), 1, 4);
	}
	if (yaml_mapping_has_member(mapping, "snow-tint")) {
		const gchar *v = yaml_mapping_get_string_member(mapping, "snow-tint");

		if (v != NULL) {
			g_free(self->snow_tint);
			self->snow_tint = gowl_palette_resolve(self->palette, v);
		}
	}
	if (yaml_mapping_has_member(mapping, "snow-clarity")) {
		self->snow_clarity = CLAMP(yaml_mapping_get_double_member(
			mapping, "snow-clarity"), 0.0, 1.0);
	}
	if (yaml_mapping_has_member(mapping, "snow-opacity")) {
		self->snow_opacity = CLAMP(yaml_mapping_get_double_member(
			mapping, "snow-opacity"), 0.0, 1.0);
	}
	if (yaml_mapping_has_member(mapping, "snow-brightness")) {
		self->snow_brightness = CLAMP(yaml_mapping_get_double_member(
			mapping, "snow-brightness"), 0.2, 2.0);
	}
	if (yaml_mapping_has_member(mapping, "snow-light")) {
		self->snow_light = CLAMP(yaml_mapping_get_double_member(
			mapping, "snow-light"), -180.0, 180.0);
	}
	if (yaml_mapping_has_member(mapping, "snow-frost")) {
		self->snow_frost = CLAMP((gint)yaml_mapping_get_int_member(
			mapping, "snow-frost"), 1, 8);
	}
	if (yaml_mapping_has_member(mapping, "snow-frost-passes")) {
		self->snow_frost_passes = CLAMP((gint)yaml_mapping_get_int_member(
			mapping, "snow-frost-passes"), 1, 6);
	}
	/* The overrides on the preset, each at its sentinel until named. */
	if (yaml_mapping_has_member(mapping, "snow-flake")) {
		self->snow_flake = CLAMP(yaml_mapping_get_double_member(
			mapping, "snow-flake"), 4.0, 400.0);
	}
	if (yaml_mapping_has_member(mapping, "snow-cell")) {
		self->snow_cell = CLAMP(yaml_mapping_get_double_member(
			mapping, "snow-cell"), 16.0, 2000.0);
	}
	if (yaml_mapping_has_member(mapping, "snow-settled")) {
		self->snow_settled = CLAMP(yaml_mapping_get_double_member(
			mapping, "snow-settled"), 0.0, 1.0);
	}
	if (yaml_mapping_has_member(mapping, "snow-column")) {
		self->snow_column = CLAMP(yaml_mapping_get_double_member(
			mapping, "snow-column"), 16.0, 2000.0);
	}
	if (yaml_mapping_has_member(mapping, "snow-falling")) {
		self->snow_falling = CLAMP(yaml_mapping_get_double_member(
			mapping, "snow-falling"), 0.0, 1.0);
	}
	if (yaml_mapping_has_member(mapping, "snow-arms")) {
		self->snow_arms = CLAMP(yaml_mapping_get_double_member(
			mapping, "snow-arms"), 0.0, 1.0);
	}
	if (yaml_mapping_has_member(mapping, "snow-drift")) {
		self->snow_drift = CLAMP(yaml_mapping_get_double_member(
			mapping, "snow-drift"), -2000.0, 2000.0);
	}
	if (yaml_mapping_has_member(mapping, "snow-flutter")) {
		self->snow_flutter = CLAMP(yaml_mapping_get_double_member(
			mapping, "snow-flutter"), 0.0, 0.40);
	}
	if (yaml_mapping_has_member(mapping, "snow-spin")) {
		self->snow_spin = CLAMP(yaml_mapping_get_double_member(
			mapping, "snow-spin"), 0.0, 8.0);
	}
	if (yaml_mapping_has_member(mapping, "snow-melt")) {
		self->snow_melt = CLAMP(yaml_mapping_get_double_member(
			mapping, "snow-melt"), 0.0, 0.80);
	}
	if (yaml_mapping_has_member(mapping, "snow-shrink")) {
		self->snow_shrink = CLAMP(yaml_mapping_get_double_member(
			mapping, "snow-shrink"), 0.05, 1.0);
	}
	if (yaml_mapping_has_member(mapping, "snow-depth")) {
		self->snow_depth = CLAMP(yaml_mapping_get_double_member(
			mapping, "snow-depth"), 0.0, 20.0);
	}
	if (yaml_mapping_has_member(mapping, "snow-runs")) {
		self->snow_runs = CLAMP(yaml_mapping_get_double_member(
			mapping, "snow-runs"), 0.0, 1.0);
	}
	if (yaml_mapping_has_member(mapping, "snow-run-width")) {
		self->snow_run_width = CLAMP(yaml_mapping_get_double_member(
			mapping, "snow-run-width"), 10.0, 600.0);
	}
	if (yaml_mapping_has_member(mapping, "snow-run-length")) {
		self->snow_run_length = CLAMP(yaml_mapping_get_double_member(
			mapping, "snow-run-length"), 0.0, 4000.0);
	}
	if (yaml_mapping_has_member(mapping, "snow-beads")) {
		self->snow_beads = CLAMP(yaml_mapping_get_double_member(
			mapping, "snow-beads"), 0.0, 1.0);
	}
	if (yaml_mapping_has_member(mapping, "snow-ice")) {
		self->snow_ice = CLAMP(yaml_mapping_get_double_member(
			mapping, "snow-ice"), 0.0, 1.0);
	}
	if (yaml_mapping_has_member(mapping, "snow-ice-rate")) {
		self->snow_ice_rate = CLAMP(yaml_mapping_get_double_member(
			mapping, "snow-ice-rate"), 0.0, 1.0);
	}
	if (yaml_mapping_has_member(mapping, "snow-ice-scale")) {
		self->snow_ice_scale = CLAMP(yaml_mapping_get_double_member(
			mapping, "snow-ice-scale"), 4.0, 400.0);
	}
	if (yaml_mapping_has_member(mapping, "snow-sparkle")) {
		self->snow_sparkle = CLAMP(yaml_mapping_get_double_member(
			mapping, "snow-sparkle"), 0.0, 2.0);
	}
	if (yaml_mapping_has_member(mapping, "snow-fog")) {
		self->snow_fog = CLAMP(yaml_mapping_get_double_member(
			mapping, "snow-fog"), 0.0, 1.0);
	}
	if (yaml_mapping_has_member(mapping, "snow-glow")) {
		self->snow_glow = CLAMP(yaml_mapping_get_double_member(
			mapping, "snow-glow"), 0.0, 3.0);
	}
	if (yaml_mapping_has_member(mapping, "snow-specular")) {
		self->snow_specular = CLAMP(yaml_mapping_get_double_member(
			mapping, "snow-specular"), 0.0, 3.0);
	}
	if (yaml_mapping_has_member(mapping, "snow-speed")) {
		self->snow_speed = CLAMP(yaml_mapping_get_double_member(
			mapping, "snow-speed"), 0.0, 5.0);
	}
	if (yaml_mapping_has_member(mapping, "hints-keys")) {
		const gchar *v = yaml_mapping_get_string_member(mapping, "hints-keys");

		/*
		 * Rejected rather than sanitised when it is unusable.  An
		 * alphabet with a repeated character would give two windows the
		 * same label, and one that is empty would give none at all --
		 * both of which look like the overlay is broken rather than
		 * like the config is.
		 */
		if (v != NULL && gowl_config_hints_keys_valid(v)) {
			g_free(self->hints_keys);
			self->hints_keys = g_ascii_strdown(v, -1);
		} else {
			g_warning("gowl_config: hints-keys '%s' is empty, too short "
			          "or has a repeated character; keeping the default",
			          v != NULL ? v : "(null)");
		}
	}
	if (yaml_mapping_has_member(mapping, "hints-colors")) {
		const gchar *v = yaml_mapping_get_string_member(mapping,
		                                                "hints-colors");
		if (v != NULL && *v != '\0') {
			g_free(self->hints_colors);
			self->hints_colors = g_strdup(v);
		}
	}
	if (yaml_mapping_has_member(mapping, "hints-timeout")) {
		/* 0 waits for a keystroke. */
		self->hints_timeout = CLAMP((gint)yaml_mapping_get_int_member(
			mapping, "hints-timeout"), 0, 60000);
	}
	if (yaml_mapping_has_member(mapping, "hints-size")) {
		self->hints_size = CLAMP((gint)yaml_mapping_get_int_member(
			mapping, "hints-size"), 16, 512);
	}
	if (yaml_mapping_has_member(mapping, "hints-border-width")) {
		self->hints_border_width = CLAMP((gint)yaml_mapping_get_int_member(
			mapping, "hints-border-width"), 0, 32);
	}
	if (yaml_mapping_has_member(mapping, "hints-scrim")) {
		self->hints_scrim = CLAMP(yaml_mapping_get_double_member(
			mapping, "hints-scrim"), 0.0, 1.0);
	}
	if (yaml_mapping_has_member(mapping, "hints-warp-pointer")) {
		self->hints_warp_pointer = yaml_mapping_get_boolean_member(
			mapping, "hints-warp-pointer");
	}
	if (yaml_mapping_has_member(mapping, "hints-current-output")) {
		self->hints_current_output = yaml_mapping_get_boolean_member(
			mapping, "hints-current-output");
	}
	/*
	 * `no-fx-apps' takes either shape, because both are the obvious
	 * one depending on how long the list is:
	 *
	 *   no-fx-apps: "obs, vlc"
	 *   no-fx-apps: [obs, vlc]
	 *
	 * Stored as one comma-separated string either way; the matcher
	 * strips whitespace around each entry, so the join needs no care.
	 */
	if (yaml_mapping_has_member(mapping, "no-fx-apps")) {
		YamlSequence *seq = yaml_mapping_get_sequence_member(mapping,
		                                                     "no-fx-apps");

		if (seq != NULL) {
			guint    len = yaml_sequence_get_length(seq);
			guint    i;
			GString *joined = g_string_new(NULL);

			for (i = 0; i < len; i++) {
				const gchar *elem = yaml_sequence_get_string_element(seq, i);

				if (elem == NULL || *elem == '\0')
					continue;
				if (joined->len > 0)
					g_string_append_c(joined, ',');
				g_string_append(joined, elem);
			}
			{
				g_autofree gchar *list = g_string_free(joined, FALSE);

				g_object_set(self, "no-fx-apps", list, NULL);
			}
		} else {
			const gchar *v = yaml_mapping_get_string_member(mapping,
			                                                "no-fx-apps");
			if (v != NULL)
				g_object_set(self, "no-fx-apps", v, NULL);
		}
	}
	if (yaml_mapping_has_member(mapping, "shadow-color")) {
		const gchar *v = yaml_mapping_get_string_member(mapping,
		                                                "shadow-color");
		if (v != NULL) {
			g_free(self->shadow_color);
			self->shadow_color = gowl_palette_resolve(self->palette, v);
		}
	}

	/* Wallpaper cross-fade and per-tag overrides.
	 *
	 * `wallpaper' keeps its meaning as the one every tag uses; these are
	 * overrides on top of it, so an existing config keeps working and a
	 * tag with no entry simply shows the default. */
	if (yaml_mapping_has_member(mapping, "wallpaper-fade")) {
		gint v = (gint)yaml_mapping_get_int_member(mapping, "wallpaper-fade");
		if (v >= 0)
			self->wallpaper_fade = v;
	}
	if (yaml_mapping_has_member(mapping, "wallpaper-tags")) {
		YamlMapping *tag_map =
			yaml_mapping_get_mapping_member(mapping, "wallpaper-tags");

		if (tag_map != NULL) {
			guint n = yaml_mapping_get_size(tag_map);
			guint ti;

			for (ti = 0; ti < n; ti++) {
				const gchar *key = yaml_mapping_get_key(tag_map, ti);
				YamlNode *val = yaml_mapping_get_value(tag_map, ti);
				const gchar *path;
				gint64 tag;

				if (key == NULL || val == NULL)
					continue;
				path = yaml_node_get_scalar(val);
				if (path == NULL)
					continue;

				/* Keys are the tag numbers the user sees in the bar, so
				 * 1-based; anything outside the range is a typo and is
				 * skipped rather than silently landing on tag 1. */
				tag = g_ascii_strtoll(key, NULL, 10);
				if (tag < 1 || tag > GOWL_CONFIG_MAX_TAGS) {
					g_warning("gowl_config: wallpaper-tags key '%s' is not "
					          "a tag number between 1 and %d", key,
					          GOWL_CONFIG_MAX_TAGS);
					continue;
				}
				g_free(self->wallpaper_tags[tag - 1]);
				self->wallpaper_tags[tag - 1] = g_strdup(path);
			}
		}
	}

	/* Per-output wallpapers.
	 *
	 * Two displays of different shapes cannot honestly share one
	 * picture: `fill' centre-crops a 16:9 image on a 21:9 panel and
	 * throws away a third of it, and `fit' letterboxes the other way
	 * round.  A key here names an output the way `monitors:' does and
	 * gives it a picture of its own -- and optionally its own mode, so
	 * one screen can fill while another fits.
	 *
	 * The value is either a bare path or a mapping with `path' and
	 * `mode'. */
	/* Locking.
	 *
	 * `lock-command' is a separate program, the way i3lock and swaylock
	 * are separate programs, and for the same reason: it takes the
	 * password, so it should not be running inside the compositor --
	 * which under cmacs --gowl is the editor, with an Elisp evaluator
	 * and a D-Bus interface attached to it.  A lock client that crashes
	 * leaves the session locked, which is the safe way round; the
	 * compositor restarts it.  Set it to "" to use the built-in
	 * screenlock module instead. */
	if (yaml_mapping_has_member(mapping, "lock-command")) {
		const gchar *v = yaml_mapping_get_string_member(mapping,
		                                                "lock-command");
		if (v != NULL) {
			g_free(self->lock_command);
			self->lock_command = g_strdup(v);
		}
	}
	if (yaml_mapping_has_member(mapping, "lock-on-suspend"))
		self->lock_on_suspend = yaml_mapping_get_boolean_member(
			mapping, "lock-on-suspend");

	if (yaml_mapping_has_member(mapping, "wallpaper-outputs")) {
		YamlMapping *out_map =
			yaml_mapping_get_mapping_member(mapping, "wallpaper-outputs");

		if (out_map != NULL) {
			guint n = yaml_mapping_get_size(out_map);
			guint oi;

			for (oi = 0; oi < n; oi++) {
				const gchar *key = yaml_mapping_get_key(out_map, oi);
				YamlNode *val = yaml_mapping_get_value(out_map, oi);
				const gchar *path = NULL;
				const gchar *mode = NULL;

				if (key == NULL || val == NULL)
					continue;

				path = yaml_node_get_scalar(val);
				if (path == NULL) {
					YamlMapping *m = yaml_node_get_mapping(val);

					if (m != NULL) {
						path = yaml_mapping_get_string_member(m, "path");
						mode = yaml_mapping_get_string_member(m, "mode");
					}
				}
				if (path == NULL) {
					g_warning("gowl_config: wallpaper-outputs '%s' has "
					          "no path", key);
					continue;
				}
				gowl_config_set_wallpaper_output(self, key, path, mode);
			}
		}
	}

	if (yaml_mapping_has_member(mapping, "animation-curve")) {
		const gchar *v = yaml_mapping_get_string_member(
			mapping, "animation-curve");
		if (v != NULL) {
			g_free(self->animation_curve);
			self->animation_curve = g_strdup(v);
		}
	}

	if (yaml_mapping_has_member(mapping, "scroll-column-width")) {
		self->scroll_column_width = yaml_mapping_get_double_member(
			mapping, "scroll-column-width");
	}

	if (yaml_mapping_has_member(mapping, "mfact")) {
		gdouble val = yaml_mapping_get_double_member(mapping, "mfact");
		g_object_set(self, "mfact", val, NULL);
	}
	if (yaml_mapping_has_member(mapping, "nmaster")) {
		gint64 val = yaml_mapping_get_int_member(mapping, "nmaster");
		g_object_set(self, "nmaster", (gint)val, NULL);
	}
	if (yaml_mapping_has_member(mapping, "tag-count")) {
		gint64 val = yaml_mapping_get_int_member(mapping, "tag-count");
		g_object_set(self, "tag-count", (gint)val, NULL);
	}
	if (yaml_mapping_has_member(mapping, "repeat-rate")) {
		gint64 val = yaml_mapping_get_int_member(mapping, "repeat-rate");
		g_object_set(self, "repeat-rate", (gint)val, NULL);
	}
	if (yaml_mapping_has_member(mapping, "repeat-delay")) {
		gint64 val = yaml_mapping_get_int_member(mapping, "repeat-delay");
		g_object_set(self, "repeat-delay", (gint)val, NULL);
	}
	if (yaml_mapping_has_member(mapping, "terminal")) {
		const gchar *val = yaml_mapping_get_string_member(mapping, "terminal");
		if (val != NULL)
			g_object_set(self, "terminal", val, NULL);
	}
	if (yaml_mapping_has_member(mapping, "menu")) {
		const gchar *val = yaml_mapping_get_string_member(mapping, "menu");
		if (val != NULL)
			g_object_set(self, "menu", val, NULL);
	}
	if (yaml_mapping_has_member(mapping, "sloppyfocus")) {
		gboolean val = yaml_mapping_get_boolean_member(mapping, "sloppyfocus");
		g_object_set(self, "sloppyfocus", val, NULL);
	}
	if (yaml_mapping_has_member(mapping, "manage_lid")) {
		gboolean val = yaml_mapping_get_boolean_member(mapping, "manage_lid");
		g_object_set(self, "manage-lid", val, NULL);
	}
	if (yaml_mapping_has_member(mapping, "idle-timeout")) {
		gint val = (gint)yaml_mapping_get_int_member(mapping, "idle-timeout");
		g_object_set(self, "idle-timeout", val, NULL);
	}
	if (yaml_mapping_has_member(mapping, "dpms-timeout")) {
		gint val = (gint)yaml_mapping_get_int_member(mapping, "dpms-timeout");
		g_object_set(self, "dpms-timeout", val, NULL);
	}
	if (yaml_mapping_has_member(mapping, "hdr-unmanaged")) {
		self->hdr_unmanaged = yaml_mapping_get_boolean_member(
			mapping, "hdr-unmanaged");
	}
	if (yaml_mapping_has_member(mapping, "hdr-advertise-pq")) {
		self->hdr_advertise_pq = yaml_mapping_get_boolean_member(
			mapping, "hdr-advertise-pq");
	}
	if (yaml_mapping_has_member(mapping, "hdr-encode")) {
		self->hdr_encode = yaml_mapping_get_boolean_member(
			mapping, "hdr-encode");
	}
	if (yaml_mapping_has_member(mapping, "hdr-bpc")) {
		gint v = (gint)yaml_mapping_get_int_member(mapping, "hdr-bpc");

		/* 8 or 10; anything else means "ask for ten, settle for
		 * eight", which is what 0 says and what the default is. */
		self->hdr_bpc = (v == 8 || v == 10) ? v : 0;
	}
	if (yaml_mapping_has_member(mapping, "hdr-sdr-white")) {
		/* Below about 40 the desktop is unreadable and above a few
		 * hundred it is the uncorrected picture again. */
		self->hdr_sdr_white = CLAMP(yaml_mapping_get_double_member(
			mapping, "hdr-sdr-white"), 40.0, 600.0);
	}
	if (yaml_mapping_has_member(mapping, "allow-tearing")) {
		gboolean val = yaml_mapping_get_boolean_member(mapping, "allow-tearing");
		g_object_set(self, "allow-tearing", val, NULL);
	}
	if (yaml_mapping_has_member(mapping, "focus-on-activate")) {
		const gchar *val = yaml_mapping_get_string_member(mapping,
			"focus-on-activate");
		if (val != NULL)
			g_object_set(self, "focus-on-activate", val, NULL);
	}
	if (yaml_mapping_has_member(mapping, "input-recording")) {
		gboolean val = yaml_mapping_get_boolean_member(mapping,
			"input-recording");
		g_object_set(self, "input-recording", val, NULL);
	}
	if (yaml_mapping_has_member(mapping, "input-recording-deny-apps")) {
		const gchar *val = yaml_mapping_get_string_member(mapping,
			"input-recording-deny-apps");
		if (val != NULL)
			g_object_set(self, "input-recording-deny-apps",
			             val, NULL);
	}
	if (yaml_mapping_has_member(mapping, "log-level")) {
		const gchar *val = yaml_mapping_get_string_member(mapping, "log-level");
		if (val != NULL)
			g_object_set(self, "log-level", val, NULL);
	}
	if (yaml_mapping_has_member(mapping, "log-file")) {
		const gchar *val = yaml_mapping_get_string_member(mapping, "log-file");
		if (val != NULL)
			g_object_set(self, "log-file", val, NULL);
	}

	/* cmacs evaluation gates.  Accept both snake_case (matches the
	 * symbol form used in C configs) and kebab-case (matches every
	 * other YAML key).  First match wins. */
	{
		const gchar *evaluate_gowl_keys[] = {
			"evaluate_gowl_config_with_cmacs",
			"evaluate-gowl-config-with-cmacs",
			NULL
		};
		const gchar *evaluate_c_keys[] = {
			"evaluate_c_config_with_cmacs",
			"evaluate-c-config-with-cmacs",
			NULL
		};
		guint k;

		for (k = 0; evaluate_gowl_keys[k] != NULL; k++) {
			if (yaml_mapping_has_member(mapping, evaluate_gowl_keys[k])) {
				gboolean val = yaml_mapping_get_boolean_member(
					mapping, evaluate_gowl_keys[k]);
				g_object_set(self,
				             "evaluate-gowl-config-with-cmacs",
				             val, NULL);
				break;
			}
		}
		for (k = 0; evaluate_c_keys[k] != NULL; k++) {
			if (yaml_mapping_has_member(mapping, evaluate_c_keys[k])) {
				gboolean val = yaml_mapping_get_boolean_member(
					mapping, evaluate_c_keys[k]);
				g_object_set(self,
				             "evaluate-c-config-with-cmacs",
				             val, NULL);
				break;
			}
		}
	}

	/* Keyboard layout.  Each is independently optional; an unset one
	 * falls back to the XKB_DEFAULT_* environment and then to xkb's
	 * own defaults, which is what happened before these keys existed. */
	{
		static const gchar *const xkb_keys[] = {
			"xkb-layout", "xkb-variant", "xkb-model",
			"xkb-options", "xkb-rules", "xkb-file"
		};
		gsize xi;

		for (xi = 0; xi < G_N_ELEMENTS(xkb_keys); xi++) {
			if (yaml_mapping_has_member(mapping, xkb_keys[xi])) {
				const gchar *val = yaml_mapping_get_string_member(
					mapping, xkb_keys[xi]);
				g_object_set(self, xkb_keys[xi],
				             (val != NULL && *val != '\0') ? val : NULL,
				             NULL);
			}
		}
	}

	/* Key modes: mapping of mode name to a keybinds mapping.  Every
	 * bind inside belongs to that mode; `mode: X' inside an ordinary
	 * keybinds entry does the same for one bind.
	 *
	 *   modes:
	 *     resize:
	 *       "h": { action: set_mfact, arg: "-0.05" }
	 *       "Escape": { action: mode, arg: default }
	 */
	if (yaml_mapping_has_member(mapping, "modes")) {
		YamlMapping *modes = yaml_mapping_get_mapping_member(mapping, "modes");
		guint mcount = modes != NULL ? yaml_mapping_get_size(modes) : 0;
		guint mi;

		for (mi = 0; mi < mcount; mi++) {
			const gchar *mode_name = yaml_mapping_get_key(modes, mi);
			YamlNode *mode_node = yaml_mapping_get_value(modes, mi);
			YamlMapping *binds;

			if (mode_name == NULL || mode_node == NULL)
				continue;
			binds = yaml_node_get_mapping(mode_node);
			if (binds != NULL)
				gowl_config_load_keybind_mapping(self, binds, mode_name);
		}
	}

	/* Pointer binds: mapping of
	 *   "Mod+Button": { action: <name>, arg: "<value>", desc: "<text>" }
	 * Buttons: Button1..9, Left/Middle/Right/Side/Extra, WheelUp/Down/
	 * Left/Right.  A section that names Super+Button1 or Super+Button3
	 * replaces the shipped move/resize grab on that button. */
	if (yaml_mapping_has_member(mapping, "mousebinds")) {
		YamlMapping *mb_mapping = yaml_mapping_get_mapping_member(
			mapping, "mousebinds");
		guint mb_count = mb_mapping != NULL ? yaml_mapping_get_size(mb_mapping) : 0;
		guint i;

		for (i = 0; i < mb_count; i++) {
			const gchar *bind_str = yaml_mapping_get_key(mb_mapping, i);
			YamlNode *val_node = yaml_mapping_get_value(mb_mapping, i);
			YamlMapping *val_map;
			gint action;
			const gchar *arg_str, *desc_str;
			guint mods, button;

			if (bind_str == NULL || val_node == NULL)
				continue;
			val_map = yaml_node_get_mapping(val_node);
			if (val_map == NULL)
				continue;
			if (!yaml_action_entry(self, val_map, &action, &arg_str, &desc_str))
				continue;
			if (!gowl_mousebind_parse(bind_str, &mods, &button))
				continue;
			gowl_config_add_mousebind(self, mods, button, action,
			                          arg_str, desc_str);
		}
	}

	/* Gesture binds: mapping of
	 *   "swipe-left-3": { action: <name>, arg: "<value>" } */
	if (yaml_mapping_has_member(mapping, "gestures")) {
		YamlMapping *g_mapping = yaml_mapping_get_mapping_member(
			mapping, "gestures");
		guint g_count = g_mapping != NULL ? yaml_mapping_get_size(g_mapping) : 0;
		guint i;

		for (i = 0; i < g_count; i++) {
			const gchar *bind_str = yaml_mapping_get_key(g_mapping, i);
			YamlNode *val_node = yaml_mapping_get_value(g_mapping, i);
			YamlMapping *val_map;
			gint action;
			const gchar *arg_str, *desc_str;
			GowlGestureKind kind;
			GowlGestureDirection dir;
			guint fingers;

			if (bind_str == NULL || val_node == NULL)
				continue;
			val_map = yaml_node_get_mapping(val_node);
			if (val_map == NULL)
				continue;
			if (!yaml_action_entry(self, val_map, &action, &arg_str, &desc_str))
				continue;
			if (!gowl_gesture_parse(bind_str, &kind, &dir, &fingers))
				continue;
			gowl_config_add_gesture(self, kind, dir, fingers, action,
			                        arg_str, desc_str);
		}
	}

	/* Per-device input settings: mapping of a device match to a
	 * mapping of settings, kept as strings.
	 *
	 *   input:
	 *     touchpad:
	 *       tap: true
	 *       natural-scroll: true
	 *     "*TrackPoint*":
	 *       accel-profile: flat
	 */
	if (yaml_mapping_has_member(mapping, "input")) {
		YamlMapping *in_mapping = yaml_mapping_get_mapping_member(
			mapping, "input");
		guint in_count = in_mapping != NULL ? yaml_mapping_get_size(in_mapping) : 0;
		guint i;

		for (i = 0; i < in_count; i++) {
			const gchar *match = yaml_mapping_get_key(in_mapping, i);
			YamlNode *val_node = yaml_mapping_get_value(in_mapping, i);
			YamlMapping *settings;
			guint n, si;

			if (match == NULL || val_node == NULL)
				continue;
			settings = yaml_node_get_mapping(val_node);
			if (settings == NULL)
				continue;
			check_known_keys(self, settings, "input", input_keys);
			n = yaml_mapping_get_size(settings);
			for (si = 0; si < n; si++) {
				const gchar *key = yaml_mapping_get_key(settings, si);
				YamlNode *vn = yaml_mapping_get_value(settings, si);
				const gchar *val = vn != NULL ? yaml_node_get_string(vn) : NULL;

				if (key != NULL && val != NULL)
					gowl_config_add_input_setting(self, match, key, val);
			}
		}
	}

	/* Keybinds: mapping of
	 *   "Mod+Key": { action: <name>, arg: "<value>", desc: "<text>" }
	 *
	 * Example:
	 *   "Super+Return": { action: spawn, arg: "gst", desc: "Terminal" }
	 *   "Super+Shift+q": { action: quit }
	 *
	 * "desc" is optional and never affects dispatch; it is what
	 * a cheatsheet renders instead of an action number.
	 */
	if (yaml_mapping_has_member(mapping, "keybinds")) {
		YamlMapping *kb_mapping = yaml_mapping_get_mapping_member(
			mapping, "keybinds");
		if (kb_mapping != NULL) {
			guint kb_count = yaml_mapping_get_size(kb_mapping);
			guint i;

			for (i = 0; i < kb_count; i++) {
				const gchar *bind_str = yaml_mapping_get_key(kb_mapping, i);
				YamlNode *val_node = yaml_mapping_get_value(kb_mapping, i);
				YamlMapping *val_map;
				const gchar *action_str;
				const gchar *arg_str;
				const gchar *desc_str;
				guint mods;
				guint keysym;
				GEnumClass *action_class;
				GEnumValue *enum_val;
				gint action;

				if (bind_str == NULL || val_node == NULL)
					continue;

				val_map = yaml_node_get_mapping(val_node);
				if (val_map == NULL)
					continue;

				action_str = yaml_mapping_get_string_member(val_map, "action");
				if (action_str == NULL)
					continue;

				arg_str = NULL;
				if (yaml_mapping_has_member(val_map, "arg"))
					arg_str = yaml_mapping_get_string_member(val_map, "arg");

				desc_str = NULL;
				if (yaml_mapping_has_member(val_map, "desc"))
					desc_str = yaml_mapping_get_string_member(val_map, "desc");

				/* Parse bind string into modifiers + keysym */
				mods = 0;
				keysym = 0;
				if (!gowl_keybind_parse(bind_str, &mods, &keysym)) {
					g_warning("gowl_config: failed to parse keybind '%s'",
					          bind_str);
					continue;
				}

				/* Resolve action name to GowlAction enum.
				 * Normalise underscores to hyphens so that both
				 * "kill_client" and "kill-client" map to the nick.
				 */
				action_class = (GEnumClass *)g_type_class_ref(
					gowl_action_get_type());
				{
					g_autofree gchar *norm = g_strdup(action_str);
					g_strdelimit(norm, "_", '-');
					enum_val = g_enum_get_value_by_nick(action_class, norm);
				}
				action = GOWL_ACTION_NONE;
				if (enum_val != NULL)
					action = enum_val->value;
				else {
					g_warning("gowl_config: unknown action '%s'", action_str);
					self->problems++;
				}
				g_type_class_unref(action_class);

				g_debug("gowl_config: keybind '%s' -> mods=0x%x sym=0x%x action=%d",
			        bind_str, mods, keysym, action);
			check_known_keys(self, val_map, "keybinds", bind_keys);
			gowl_config_add_keybind_ex(self, mods, keysym, action,
			                           arg_str, desc_str,
			                           yaml_keybind_mode(val_map, NULL),
			                           yaml_keybind_flags(val_map));
			}
		}
	}

	/* Rules: expect a sequence of mappings with keys:
	 *   app-id: "firefox"          (optional)
	 *   title: ".*"                (optional)
	 *   tags: 2                    (optional, default 0)
	 *   floating: true             (optional, default false)
	 *   monitor: 0                 (optional, default -1)
	 */
	if (yaml_mapping_has_member(mapping, "rules")) {
		YamlSequence *seq = yaml_mapping_get_sequence_member(mapping, "rules");
		if (seq != NULL) {
			guint len = yaml_sequence_get_length(seq);
			guint i;

			for (i = 0; i < len; i++) {
				YamlMapping *rule_map = yaml_sequence_get_mapping_element(seq, i);
				const gchar *app_id;
				const gchar *title;
				guint32 tags;
				gboolean floating;
				gint monitor;
				gint width;
				gint height;
				gboolean center;
				gboolean regex_mode;

				if (rule_map == NULL)
					continue;

				app_id = NULL;
				title = NULL;
				tags = 0;
				floating = FALSE;
				monitor = -1;
				width = 0;
				height = 0;
				center = TRUE;
				regex_mode = FALSE;

				if (yaml_mapping_has_member(rule_map, "app-id"))
					app_id = yaml_mapping_get_string_member(rule_map, "app-id");
				if (yaml_mapping_has_member(rule_map, "title"))
					title = yaml_mapping_get_string_member(rule_map, "title");
				if (yaml_mapping_has_member(rule_map, "tags"))
					tags = (guint32)yaml_mapping_get_int_member(rule_map, "tags");
				if (yaml_mapping_has_member(rule_map, "floating"))
					floating = yaml_mapping_get_boolean_member(rule_map, "floating");
				if (yaml_mapping_has_member(rule_map, "monitor"))
					monitor = (gint)yaml_mapping_get_int_member(rule_map, "monitor");
				if (yaml_mapping_has_member(rule_map, "width"))
					width = (gint)yaml_mapping_get_int_member(rule_map, "width");
				if (yaml_mapping_has_member(rule_map, "height"))
					height = (gint)yaml_mapping_get_int_member(rule_map, "height");
				if (yaml_mapping_has_member(rule_map, "center"))
					center = yaml_mapping_get_boolean_member(rule_map, "center");
				if (yaml_mapping_has_member(rule_map, "regex"))
					regex_mode = yaml_mapping_get_boolean_member(rule_map, "regex");

				gowl_config_add_rule_full(self, app_id, title, tags,
				                           floating, monitor,
				                           width, height, center,
				                           regex_mode);
				{
					GowlRuleEntry *added = g_ptr_array_index(
						self->rules, self->rules->len - 1);

					if (yaml_mapping_has_member(rule_map, "sticky"))
						added->sticky = yaml_mapping_get_boolean_member(
							rule_map, "sticky");
					if (yaml_mapping_has_member(rule_map, "initial-title"))
						added->initial_title = g_strdup(
							yaml_mapping_get_string_member(rule_map,
								"initial-title"));
					if (yaml_mapping_has_member(rule_map, "xwayland"))
						added->xwayland = yaml_mapping_get_boolean_member(
							rule_map, "xwayland") ? 1 : 0;
					if (yaml_mapping_has_member(rule_map, "pid"))
						added->pid = (gint)yaml_mapping_get_int_member(
							rule_map, "pid");
					if (yaml_mapping_has_member(rule_map, "no-focus"))
						added->no_focus = yaml_mapping_get_boolean_member(
							rule_map, "no-focus");
					if (yaml_mapping_has_member(rule_map, "fullscreen"))
						added->fullscreen = yaml_mapping_get_boolean_member(
							rule_map, "fullscreen");
					if (yaml_mapping_has_member(rule_map, "opacity")) {
						gdouble o = yaml_mapping_get_double_member(
							rule_map, "opacity");
						if (o < 0.05 || o > 1.0) {
							g_warning("gowl_config: rule opacity %.2f "
							          "is outside 0.05..1.0; ignored", o);
							self->problems++;
						} else {
							added->opacity = o;
						}
					}
					if (yaml_mapping_has_member(rule_map, "no-blur"))
						added->no_blur = yaml_mapping_get_boolean_member(
							rule_map, "no-blur");
					if (yaml_mapping_has_member(rule_map, "no-shadow"))
						added->no_shadow = yaml_mapping_get_boolean_member(
							rule_map, "no-shadow");
					if (yaml_mapping_has_member(rule_map, "no-anim"))
						added->no_anim = yaml_mapping_get_boolean_member(
							rule_map, "no-anim");
					if (yaml_mapping_has_member(rule_map, "idle-inhibit"))
						added->idle_inhibit = yaml_mapping_get_boolean_member(
							rule_map, "idle-inhibit");
					/* State matchers.  `floating:' and `fullscreen:'
					 * are what the rule DOES; `is-floating:' and
					 * `is-fullscreen:' are what the window must
					 * already be for the rule to apply at all.  Both
					 * are tri-state, so an absent key keeps -1. */
					if (yaml_mapping_has_member(rule_map, "is-floating"))
						added->match_floating =
							yaml_mapping_get_boolean_member(
								rule_map, "is-floating") ? 1 : 0;
					if (yaml_mapping_has_member(rule_map, "is-fullscreen"))
						added->match_fullscreen =
							yaml_mapping_get_boolean_member(
								rule_map, "is-fullscreen") ? 1 : 0;
					if (yaml_mapping_has_member(rule_map, "on-tag")) {
						gint t = (gint)yaml_mapping_get_int_member(
							rule_map, "on-tag");

						if (t < 1 || t > 9) {
							g_warning("gowl_config: rules: on-tag %d is "
							          "not a tag (1-9)", t);
							self->problems++;
						} else {
							added->on_tag = t;
						}
					}
					if (yaml_mapping_has_member(rule_map, "focus"))
						added->focus = yaml_mapping_get_boolean_member(
							rule_map, "focus");
					/* Sizes as a fraction of the output's usable area,
					 * so one rule fits every screen the window lands
					 * on.  Written 0.5 or "50%"; both read here. */
					if (yaml_mapping_has_member(rule_map, "width-pct"))
						added->width_pct = gowl_parse_percent(
							yaml_mapping_get_string_member(
								rule_map, "width-pct"));
					if (yaml_mapping_has_member(rule_map, "height-pct"))
						added->height_pct = gowl_parse_percent(
							yaml_mapping_get_string_member(
								rule_map, "height-pct"));
					check_known_keys(self, rule_map, "rules", rule_keys);
				}
			}
		}
	}

	/* Dropdowns: sequence of mappings with keys:
	 *   name: "term"                 (required)
	 *   spawn-cmd: "foot"            (required)
	 *   keybind: "Super+grave"       (optional)
	 *   width-pct: 1.0               (optional, default 1.0)
	 *   height-pct: 0.666667              (optional, default two-thirds)
	 *   width: 800                   (optional, absolute px)
	 *   height: 600                  (optional, absolute px)
	 *   anchor: "top"|"bottom"|"left"|"right"  (optional, default top)
	 */
	if (yaml_mapping_has_member(mapping, "dropdowns")) {
		YamlSequence *seq = yaml_mapping_get_sequence_member(mapping,
		                                                      "dropdowns");
		if (seq != NULL) {
			guint len = yaml_sequence_get_length(seq);
			guint i;

			for (i = 0; i < len; i++) {
				YamlMapping *dd_map;
				const gchar *name;
				const gchar *spawn_cmd;
				const gchar *keybind;
				const gchar *anchor_str;
				gdouble width_pct;
				gdouble height_pct;
				gint width_abs;
				gint height_abs;
				gint anchor;

				dd_map = yaml_sequence_get_mapping_element(seq, i);
				if (dd_map == NULL)
					continue;

				name = NULL;
				spawn_cmd = NULL;
				keybind = NULL;
				anchor_str = NULL;
				width_pct = 1.0;
				height_pct = 2.0 / 3.0;
				width_abs = 0;
				height_abs = 0;
				anchor = 0; /* top */

				if (yaml_mapping_has_member(dd_map, "name"))
					name = yaml_mapping_get_string_member(dd_map, "name");
				if (yaml_mapping_has_member(dd_map, "spawn-cmd"))
					spawn_cmd = yaml_mapping_get_string_member(dd_map, "spawn-cmd");
				if (yaml_mapping_has_member(dd_map, "keybind"))
					keybind = yaml_mapping_get_string_member(dd_map, "keybind");
				if (yaml_mapping_has_member(dd_map, "anchor"))
					anchor_str = yaml_mapping_get_string_member(dd_map, "anchor");
				if (yaml_mapping_has_member(dd_map, "width-pct"))
					width_pct = yaml_mapping_get_double_member(dd_map, "width-pct");
				if (yaml_mapping_has_member(dd_map, "height-pct"))
					height_pct = yaml_mapping_get_double_member(dd_map, "height-pct");
				if (yaml_mapping_has_member(dd_map, "width"))
					width_abs = (gint)yaml_mapping_get_int_member(dd_map, "width");
				if (yaml_mapping_has_member(dd_map, "height"))
					height_abs = (gint)yaml_mapping_get_int_member(dd_map, "height");

				if (anchor_str != NULL) {
					if (g_ascii_strcasecmp(anchor_str, "bottom") == 0)
						anchor = 1;
					else if (g_ascii_strcasecmp(anchor_str, "left") == 0)
						anchor = 2;
					else if (g_ascii_strcasecmp(anchor_str, "right") == 0)
						anchor = 3;
					else
						anchor = 0;
				}

				if (name == NULL || spawn_cmd == NULL) {
					g_warning("gowl-config: dropdown entry %u missing name or spawn-cmd", i);
					continue;
				}

				gowl_config_add_dropdown(self, name, spawn_cmd, keybind,
				                          width_pct, height_pct,
				                          width_abs, height_abs,
				                          anchor);
			}
		}
	}

	/* Modules: each top-level key is a module name, value is a mapping
	 * of setting keys to scalar values.  We flatten each module's
	 * mapping into a GHashTable<string, string> for generic consumption
	 * by the module's configure() method.
	 *
	 * Example YAML:
	 *   modules:
	 *     vanitygaps:
	 *       enabled: true
	 *       inner-h: 10
	 */
	if (yaml_mapping_has_member(mapping, "modules")) {
		YamlMapping *mod_mapping;

		mod_mapping = yaml_mapping_get_mapping_member(mapping, "modules");
		if (mod_mapping != NULL) {
			guint mod_count = yaml_mapping_get_size(mod_mapping);
			guint mi;

			/* Clear any previously-loaded module configs on reload */
			g_hash_table_remove_all(self->module_configs);

			for (mi = 0; mi < mod_count; mi++) {
				const gchar *mod_name;
				YamlNode *mod_val_node;
				YamlMapping *mod_cfg_map;
				GHashTable *settings;
				guint si, setting_count;

				mod_name = yaml_mapping_get_key(mod_mapping, mi);
				mod_val_node = yaml_mapping_get_value(mod_mapping, mi);
				if (mod_name == NULL || mod_val_node == NULL)
					continue;

				mod_cfg_map = yaml_node_get_mapping(mod_val_node);
				if (mod_cfg_map == NULL)
					continue;

				/* Build settings hash for this module */
				settings = g_hash_table_new_full(
					g_str_hash, g_str_equal, g_free, g_free);

				setting_count = yaml_mapping_get_size(mod_cfg_map);
				for (si = 0; si < setting_count; si++) {
					const gchar *key;
					YamlNode *val_node;
					const gchar *val_str;

					key = yaml_mapping_get_key(mod_cfg_map, si);
					val_node = yaml_mapping_get_value(mod_cfg_map, si);
					if (key == NULL || val_node == NULL)
						continue;

					/* Get the raw scalar string from the YAML node.
					 * For numbers and bools this is the text form
					 * (e.g. "5", "true"). */
					val_str = yaml_node_get_scalar(val_node);
					if (val_str != NULL) {
						/* A colour setting is resolved against the
						 * palette here rather than in each module.
						 * Module settings are an untyped string map
						 * with no schema, so the key name is all
						 * there is to go on --- and doing it here
						 * means a module written tomorrow gets
						 * palette support without knowing the
						 * feature exists. */
						g_hash_table_insert(settings,
						                    g_strdup(key),
						                    gowl_palette_key_is_color(key)
						                    ? gowl_palette_resolve(
						                              self->palette, val_str)
						                    : g_strdup(val_str));
					} else {
						/* Handle sequence values (e.g. commands list).
						 * Join elements with newline so modules can
						 * split them back via g_strsplit(). */
						YamlSequence *seq;

						seq = yaml_node_get_sequence(val_node);
						if (seq != NULL) {
							guint slen;
							guint si2;
							GString *joined;

							slen = yaml_sequence_get_length(seq);
							joined = g_string_new(NULL);
							for (si2 = 0; si2 < slen; si2++) {
								const gchar *elem;

								elem = yaml_sequence_get_string_element(
									seq, si2);
								if (elem == NULL)
									continue;
								if (joined->len > 0)
									g_string_append_c(joined, '\n');
								g_string_append(joined, elem);
							}
							g_hash_table_insert(settings,
							                    g_strdup(key),
							                    g_string_free(joined, FALSE));
						}
					}
				}

				g_debug("gowl_config: loaded %u settings for module '%s'",
				        g_hash_table_size(settings), mod_name);

				g_hash_table_insert(self->module_configs,
				                    g_strdup(mod_name),
				                    settings);
			}
		}
	}

	/* Monitor configs: each child of `monitors:` is keyed by output
	 * name and maps to a per-output mapping with optional fields
	 * (width, height, refresh, x, y, scale, enabled, transform).
	 * Every field is independently optional -- unset fields are
	 * left at compositor defaults.
	 *
	 * Example YAML:
	 *   monitors:
	 *     eDP-1:
	 *       transform: 90       # rotate a portrait-default tablet
	 *     HDMI-A-1:
	 *       x: 1080
	 *       scale: 1.5
	 */
	if (yaml_mapping_has_member(mapping, "monitors")) {
		YamlMapping *mon_mapping;

		mon_mapping = yaml_mapping_get_mapping_member(mapping,
		                                               "monitors");
		if (mon_mapping != NULL) {
			guint mon_count = yaml_mapping_get_size(mon_mapping);
			guint mi;

			/* Clear any previously-loaded monitor configs on reload */
			g_hash_table_remove_all(self->monitor_configs);

			for (mi = 0; mi < mon_count; mi++) {
				const gchar *mon_name;
				YamlNode *mon_val_node;
				YamlMapping *mon_cfg_map;
				GowlMonitorConfig *mc;

				mon_name = yaml_mapping_get_key(mon_mapping, mi);
				mon_val_node = yaml_mapping_get_value(mon_mapping, mi);
				if (mon_name == NULL || mon_val_node == NULL)
					continue;

				mon_cfg_map = yaml_node_get_mapping(mon_val_node);
				if (mon_cfg_map == NULL)
					continue;

				mc = parse_monitor_config(mon_cfg_map);
				check_known_keys(self, mon_cfg_map, "monitors", monitor_keys);
				g_debug("gowl_config: monitor '%s': "
				        "w=%d h=%d refresh=%.1f x=%d y=%d "
				        "scale=%.2f transform=%d enabled=%d",
				        mon_name, mc->width, mc->height,
				        mc->refresh, mc->x, mc->y,
				        mc->scale, mc->transform, mc->enabled);

				g_hash_table_insert(self->monitor_configs,
				                    g_strdup(mon_name), mc);
			}
		}
	}

	parse_output_profiles(self, mapping);
}

/**
 * gowl_config_load_yaml:
 * @self: a #GowlConfig
 * @path: filesystem path to a YAML configuration file
 * @error: (nullable): return location for a #GError
 *
 * Parses the YAML file at @path using yaml-glib and applies the
 * top-level mapping to the config properties.
 *
 * Returns: %TRUE on success, %FALSE on error
 */
gboolean
gowl_config_load_yaml(
	GowlConfig   *self,
	const gchar  *path,
	GError      **error
){
	g_autoptr(YamlParser) parser = NULL;
	YamlNode *root = NULL;
	YamlMapping *mapping = NULL;

	g_return_val_if_fail(GOWL_IS_CONFIG(self), FALSE);
	g_return_val_if_fail(path != NULL, FALSE);

	parser = yaml_parser_new();

	if (!yaml_parser_load_from_file(parser, path, error))
		return FALSE;

	root = yaml_parser_get_root(parser);
	if (root == NULL) {
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
		            "YAML file '%s' has no root node", path);
		return FALSE;
	}

	if (yaml_node_get_node_type(root) != YAML_NODE_MAPPING) {
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
		            "YAML file '%s' root is not a mapping", path);
		return FALSE;
	}

	mapping = yaml_node_get_mapping(root);
	if (mapping == NULL) {
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
		            "YAML file '%s' contains a null mapping", path);
		return FALSE;
	}

	/* check ignore_yaml: if true, discard everything and keep defaults */
	if (yaml_mapping_has_member(mapping, "ignore_yaml")) {
		if (yaml_mapping_get_boolean_member(mapping, "ignore_yaml")) {
			g_debug("gowl_config: ignore_yaml set, keeping defaults");
			return TRUE;
		}
	}

	gowl_config_apply_mapping(self, mapping);

	/* Emit the reloaded signal */
	g_signal_emit(self, signals[SIGNAL_RELOADED], 0);
	return TRUE;
}

/**
 * gowl_config_load_rules_d:
 * @self: a #GowlConfig
 * @config_path: the config file that was loaded
 *
 * Loads window rules from `rules.d/' beside @config_path, one file per
 * application, merged after the flat `rules:' list in the main config.
 *
 * A single growing `rules:' array is the wrong shape for this: a rule
 * for Steam and a rule for a screenshot overlay have nothing to do with
 * each other, and a config that ships rules cannot be edited by a user
 * without merging.  Omarchy keeps one Lua file per application for
 * exactly this reason.
 *
 * Files are read in sorted order so a numeric prefix decides
 * precedence, and each is a fragment carrying its own `rules:'
 * sequence, so it is a valid config file in its own right.
 *
 * Returns: how many files were loaded.
 */
guint
gowl_config_load_rules_d(GowlConfig *self, const gchar *config_path)
{
	g_autofree gchar *dir_path = NULL;
	g_autofree gchar *parent = NULL;
	g_autoptr(GDir) dir = NULL;
	g_autoptr(GPtrArray) files = NULL;
	const gchar *name;
	guint i, loaded = 0;

	g_return_val_if_fail(GOWL_IS_CONFIG(self), 0);
	g_return_val_if_fail(config_path != NULL, 0);

	parent = g_path_get_dirname(config_path);
	dir_path = g_build_filename(parent, "rules.d", NULL);

	dir = g_dir_open(dir_path, 0, NULL);
	if (dir == NULL)
		return 0;         /* no rules.d is the normal case */

	files = g_ptr_array_new_with_free_func(g_free);
	while ((name = g_dir_read_name(dir)) != NULL) {
		if (!g_str_has_suffix(name, ".yaml")
		    && !g_str_has_suffix(name, ".yml"))
			continue;
		g_ptr_array_add(files, g_build_filename(dir_path, name, NULL));
	}

	/* Sorted, so a `10-' prefix means what it looks like it means.
	 * Directory order is not sorted and differs between filesystems. */
	g_ptr_array_sort_values(files, (GCompareFunc)g_strcmp0);

	for (i = 0; i < files->len; i++) {
		const gchar *file = g_ptr_array_index(files, i);
		GError *err = NULL;

		/* A broken fragment costs itself, not the whole session: the
		 * rules that did parse stay, and the compositor still starts. */
		if (!gowl_config_load_yaml(self, file, &err)) {
			g_warning("gowl_config: %s: %s", file,
			          err ? err->message : "failed to load");
			g_clear_error(&err);
			continue;
		}
		g_debug("gowl_config: loaded rules from '%s'", file);
		loaded++;
	}

	return loaded;
}

/**
 * gowl_config_load_yaml_from_search_path:
 * @self: a #GowlConfig
 * @error: (nullable): return location for a #GError
 *
 * Searches standard directories for config.yaml and loads the first
 * one found. If no file exists, the config keeps its defaults and
 * the function returns %TRUE.
 *
 * Returns: %TRUE on success (including no-file-found), %FALSE on error
 */
gboolean
gowl_config_load_yaml_from_search_path(
	GowlConfig  *self,
	GError     **error
){
	g_autofree gchar *xdg_path = NULL;
	const gchar *search_paths[5];
	guint i;

	g_return_val_if_fail(GOWL_IS_CONFIG(self), FALSE);

	/* Build the XDG config path: ~/.config/gowl/config.yaml */
	xdg_path = g_build_filename(g_get_user_config_dir(),
	                             "gowl",
	                             GOWL_CONFIG_FILENAME,
	                             NULL);

	search_paths[0] = "data/" GOWL_CONFIG_FILENAME;
	search_paths[1] = xdg_path;
	search_paths[2] = GOWL_SYSCONFDIR "/gowl/" GOWL_CONFIG_FILENAME;
	search_paths[3] = GOWL_DATADIR "/gowl/" GOWL_CONFIG_FILENAME;
	search_paths[4] = NULL;

	for (i = 0; search_paths[i] != NULL; i++) {
		if (g_file_test(search_paths[i], G_FILE_TEST_EXISTS)) {
			gboolean ok;

			g_debug("gowl_config: loading config from '%s'",
			        search_paths[i]);
			ok = gowl_config_load_yaml(self, search_paths[i], error);
			if (ok)
				gowl_config_load_rules_d(self, search_paths[i]);
			return ok;
		}
	}

	g_debug("gowl_config: no config file found, using defaults");
	return TRUE;
}

/* --- YAML generation --- */

/**
 * gowl_config_generate_yaml:
 * @self: a #GowlConfig
 *
 * Builds a YAML representation of the current config state using
 * g_string_append_printf(). This is intentionally simple rather than
 * using the full yaml-glib generator, since the schema is known and fixed.
 *
 * Returns: (transfer full): a newly allocated YAML string
 */
gchar *
gowl_config_generate_yaml(GowlConfig *self)
{
	GString *yaml;
	guint i;

	g_return_val_if_fail(GOWL_IS_CONFIG(self), NULL);

	yaml = g_string_new("# gowl configuration\n\n");

	/*
	 * The palette first, and the specs below unresolved --- a generated
	 * config that baked in today's literals would be one a theme change
	 * could no longer reach, which is the state this replaced.
	 */
	{
		g_auto(GStrv) names = gowl_palette_names(self->palette_override);
		gsize i;

		g_string_append(yaml, "palette:\n");
		g_string_append_printf(yaml, "  name: %s\n", self->palette_name);
		/* Only the overrides: the flavour supplies the rest, and
		 * writing all fifteen out would freeze them. */
		for (i = 0; names != NULL && names[i] != NULL; i++) {
			g_string_append_printf(yaml, "  %s: \"%s\"\n", names[i],
			                       gowl_palette_lookup(
			                               self->palette_override, names[i]));
		}
		g_string_append_c(yaml, '\n');
	}

	/* Appearance */
	g_string_append_printf(yaml, "border-width: %d\n", self->border_width);
	g_string_append_printf(yaml, "border-color-focus: \"%s\"\n", self->border_color_focus);
	g_string_append_printf(yaml, "border-color-unfocus: \"%s\"\n", self->border_color_unfocus);
	g_string_append_printf(yaml, "border-color-urgent: \"%s\"\n", self->border_color_urgent);

	/* Layout */
	g_string_append_printf(yaml, "mfact: %.2f\n", self->mfact);
	g_string_append_printf(yaml, "nmaster: %d\n", self->nmaster);
	g_string_append_printf(yaml, "tag-count: %d\n", self->tag_count);

	/* Input */
	g_string_append_printf(yaml, "repeat-rate: %d\n", self->repeat_rate);
	g_string_append_printf(yaml, "repeat-delay: %d\n", self->repeat_delay);
	g_string_append_printf(yaml, "sloppyfocus: %s\n", self->sloppyfocus ? "true" : "false");
	g_string_append_printf(yaml, "manage_lid: %s\n", self->manage_lid ? "true" : "false");
	g_string_append_printf(yaml, "idle-timeout: %d\n", self->idle_timeout);
	g_string_append_printf(yaml, "dpms-timeout: %d\n", self->dpms_timeout);
	g_string_append_printf(yaml, "allow-tearing: %s\n",
	                       self->allow_tearing ? "true" : "false");
	g_string_append_printf(yaml, "focus-on-activate: \"%s\"\n",
	                       self->focus_on_activate != NULL
	                       ? self->focus_on_activate : "smart");
	{
		const gchar *const xkb_keys[] = {
			"xkb-layout", "xkb-variant", "xkb-model",
			"xkb-options", "xkb-rules", "xkb-file"
		};
		const gchar *const xkb_vals[] = {
			self->xkb_layout, self->xkb_variant, self->xkb_model,
			self->xkb_options, self->xkb_rules, self->xkb_file
		};
		gsize xi;

		for (xi = 0; xi < G_N_ELEMENTS(xkb_keys); xi++) {
			if (xkb_vals[xi] == NULL)
				continue;
			{
				g_autofree gchar *esc = gowl_config_escape_yaml(xkb_vals[xi]);
				g_string_append_printf(yaml, "%s: \"%s\"\n",
				                       xkb_keys[xi], esc);
			}
		}
	}
	g_string_append_printf(yaml, "input-recording: %s\n",
	                       self->input_recording ? "true" : "false");
	g_string_append_printf(yaml, "input-recording-deny-apps: \"%s\"\n",
	                       self->input_recording_deny_apps != NULL
	                       ? self->input_recording_deny_apps : "");
	g_string_append_printf(yaml, "no-fx-apps: \"%s\"\n",
	                       self->no_fx_apps != NULL ? self->no_fx_apps : "");

	/* Programs */
	g_string_append_printf(yaml, "terminal: \"%s\"\n", self->terminal);
	g_string_append_printf(yaml, "menu: \"%s\"\n", self->menu);

	/* Logging */
	g_string_append_printf(yaml, "log-level: \"%s\"\n", self->log_level);
	g_string_append_printf(yaml, "log-file: \"%s\"\n", self->log_file);

	/* cmacs evaluation gates (kebab-case; snake_case also accepted on load) */
	g_string_append_printf(yaml,
	                       "evaluate-gowl-config-with-cmacs: %s\n",
	                       self->evaluate_gowl_config_with_cmacs
	                       ? "true" : "false");
	g_string_append_printf(yaml,
	                       "evaluate-c-config-with-cmacs: %s\n",
	                       self->evaluate_c_config_with_cmacs
	                       ? "true" : "false");

	/* Keybinds.
	 *
	 * Emitted as a MAPPING of "bind": { action: ..., arg: ..., desc: ... },
	 * which is the shape gowl_config_apply_mapping() reads back.  An
	 * earlier version wrote a sequence of "- bind:" items; the parser
	 * asks for a mapping member, got NULL for a sequence and dropped
	 * every keybind silently, so a config saved from the dashboard came
	 * back with no binds at all. */
	if (self->keybinds->len > 0) {
		GEnumClass *action_class = (GEnumClass *)g_type_class_ref(
			gowl_action_get_type());

		g_string_append(yaml, "\nkeybinds:\n");
		for (i = 0; i < self->keybinds->len; i++) {
			GowlKeybindEntry *kb = &g_array_index(self->keybinds, GowlKeybindEntry, i);
			g_autofree gchar *bind_str = gowl_keybind_to_string(kb->modifiers, kb->keysym);
			GEnumValue *enum_val = g_enum_get_value(action_class, kb->action);
			const gchar *action_nick = (enum_val != NULL) ? enum_val->value_nick : "none";

			g_string_append_printf(yaml, "  \"%s\": { action: %s",
			                       bind_str, action_nick);
			if (kb->arg != NULL) {
				g_autofree gchar *esc = gowl_config_escape_yaml(kb->arg);
				g_string_append_printf(yaml, ", arg: \"%s\"", esc);
			}
			if (kb->desc != NULL) {
				g_autofree gchar *esc = gowl_config_escape_yaml(kb->desc);
				g_string_append_printf(yaml, ", desc: \"%s\"", esc);
			}
			if (kb->mode != NULL) {
				g_autofree gchar *esc = gowl_config_escape_yaml(kb->mode);
				g_string_append_printf(yaml, ", mode: \"%s\"", esc);
			}
			if (kb->flags & GOWL_KEYBIND_FLAG_LOCKED)
				g_string_append(yaml, ", locked: true");
			if (kb->flags & GOWL_KEYBIND_FLAG_RELEASE)
				g_string_append(yaml, ", release: true");
			if (kb->flags & GOWL_KEYBIND_FLAG_NO_REPEAT)
				g_string_append(yaml, ", repeat: false");
			g_string_append(yaml, " }\n");
		}

		g_type_class_unref(action_class);
	}

	/* Pointer binds -- always, since the two defaults are in here and
	 * a reader should see what Super+Button1 does. */
	if (self->mousebinds->len > 0) {
		GEnumClass *action_class = (GEnumClass *)g_type_class_ref(
			gowl_action_get_type());

		g_string_append(yaml, "\nmousebinds:\n");
		for (i = 0; i < self->mousebinds->len; i++) {
			GowlMousebindEntry *mb =
				&g_array_index(self->mousebinds, GowlMousebindEntry, i);
			g_autofree gchar *bind_str =
				gowl_mousebind_to_string(mb->modifiers, mb->button);
			GEnumValue *enum_val = g_enum_get_value(action_class, mb->action);

			g_string_append_printf(yaml, "  \"%s\": { action: %s",
			                       bind_str,
			                       enum_val != NULL ? enum_val->value_nick : "none");
			if (mb->arg != NULL) {
				g_autofree gchar *esc = gowl_config_escape_yaml(mb->arg);
				g_string_append_printf(yaml, ", arg: \"%s\"", esc);
			}
			if (mb->desc != NULL) {
				g_autofree gchar *esc = gowl_config_escape_yaml(mb->desc);
				g_string_append_printf(yaml, ", desc: \"%s\"", esc);
			}
			g_string_append(yaml, " }\n");
		}
		g_type_class_unref(action_class);
	}

	/* Gesture binds */
	if (self->gestures->len > 0) {
		GEnumClass *action_class = (GEnumClass *)g_type_class_ref(
			gowl_action_get_type());

		g_string_append(yaml, "\ngestures:\n");
		for (i = 0; i < self->gestures->len; i++) {
			GowlGestureEntry *ge =
				&g_array_index(self->gestures, GowlGestureEntry, i);
			g_autofree gchar *bind_str = gowl_gesture_to_string(
				(GowlGestureKind)ge->kind,
				(GowlGestureDirection)ge->direction, ge->fingers);
			GEnumValue *enum_val = g_enum_get_value(action_class, ge->action);

			g_string_append_printf(yaml, "  \"%s\": { action: %s",
			                       bind_str,
			                       enum_val != NULL ? enum_val->value_nick : "none");
			if (ge->arg != NULL) {
				g_autofree gchar *esc = gowl_config_escape_yaml(ge->arg);
				g_string_append_printf(yaml, ", arg: \"%s\"", esc);
			}
			if (ge->desc != NULL) {
				g_autofree gchar *esc = gowl_config_escape_yaml(ge->desc);
				g_string_append_printf(yaml, ", desc: \"%s\"", esc);
			}
			g_string_append(yaml, " }\n");
		}
		g_type_class_unref(action_class);
	}

	/* input: blocks, settings in hash order (they are independent) */
	if (self->input_configs->len > 0) {
		g_string_append(yaml, "\ninput:\n");
		for (i = 0; i < self->input_configs->len; i++) {
			GowlInputConfigEntry *ic =
				g_ptr_array_index(self->input_configs, i);
			GHashTableIter it;
			gpointer k, v;
			g_autofree gchar *esc_match = gowl_config_escape_yaml(ic->match);

			g_string_append_printf(yaml, "  \"%s\":\n", esc_match);
			g_hash_table_iter_init(&it, ic->settings);
			while (g_hash_table_iter_next(&it, &k, &v)) {
				g_autofree gchar *esc = gowl_config_escape_yaml((const gchar *)v);
				g_string_append_printf(yaml, "    %s: \"%s\"\n",
				                       (const gchar *)k, esc);
			}
		}
	}

	/* Rules */
	if (self->rules->len > 0) {
		g_string_append(yaml, "\nrules:\n");
		for (i = 0; i < self->rules->len; i++) {
			GowlRuleEntry *rule = (GowlRuleEntry *)g_ptr_array_index(self->rules, i);

			g_string_append(yaml, "  - ");
			if (rule->app_id != NULL)
				g_string_append_printf(yaml, "app-id: \"%s\"\n    ", rule->app_id);
			if (rule->title != NULL)
				g_string_append_printf(yaml, "title: \"%s\"\n    ", rule->title);
			g_string_append_printf(yaml, "tags: %u\n    ", (guint)rule->tags);
			g_string_append_printf(yaml, "floating: %s\n    ", rule->floating ? "true" : "false");
			g_string_append_printf(yaml, "monitor: %d\n", rule->monitor);
			if (rule->width != 0)
				g_string_append_printf(yaml, "    width: %d\n", rule->width);
			if (rule->height != 0)
				g_string_append_printf(yaml, "    height: %d\n", rule->height);
			if (!rule->center)
				g_string_append(yaml, "    center: false\n");
			if (rule->regex_mode)
				g_string_append(yaml, "    regex: true\n");
			if (rule->sticky)
				g_string_append(yaml, "    sticky: true\n");
			if (rule->initial_title != NULL) {
				g_autofree gchar *esc = gowl_config_escape_yaml(rule->initial_title);
				g_string_append_printf(yaml, "    initial-title: \"%s\"\n", esc);
			}
			if (rule->xwayland >= 0)
				g_string_append_printf(yaml, "    xwayland: %s\n",
				                       rule->xwayland ? "true" : "false");
			if (rule->pid > 0)
				g_string_append_printf(yaml, "    pid: %d\n", rule->pid);
			if (rule->no_focus)
				g_string_append(yaml, "    no-focus: true\n");
			if (rule->fullscreen)
				g_string_append(yaml, "    fullscreen: true\n");
			if (rule->opacity > 0.0)
				g_string_append_printf(yaml, "    opacity: %.2f\n", rule->opacity);
			if (rule->no_blur)
				g_string_append(yaml, "    no-blur: true\n");
			if (rule->no_shadow)
				g_string_append(yaml, "    no-shadow: true\n");
			if (rule->no_anim)
				g_string_append(yaml, "    no-anim: true\n");
			if (rule->idle_inhibit)
				g_string_append(yaml, "    idle-inhibit: true\n");
			if (rule->match_floating >= 0)
				g_string_append_printf(yaml, "    is-floating: %s\n",
				                       rule->match_floating ? "true" : "false");
			if (rule->match_fullscreen >= 0)
				g_string_append_printf(yaml, "    is-fullscreen: %s\n",
				                       rule->match_fullscreen
				                       ? "true" : "false");
			if (rule->on_tag > 0)
				g_string_append_printf(yaml, "    on-tag: %d\n",
				                       rule->on_tag);
			if (rule->focus)
				g_string_append(yaml, "    focus: true\n");
			if (rule->width_pct > 0.0)
				g_string_append_printf(yaml, "    width-pct: %.3f\n",
				                       rule->width_pct);
			if (rule->height_pct > 0.0)
				g_string_append_printf(yaml, "    height-pct: %.3f\n",
				                       rule->height_pct);
		}
	}

	return g_string_free(yaml, FALSE);
}

/* --- Property getters --- */

gint
gowl_config_get_border_width(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self), GOWL_CONFIG_DEFAULT_BORDER_WIDTH);
	return self->border_width;
}

const gchar *
gowl_config_get_border_color_focus(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self), "#89b4fa");
	return self->border_hex_focus;
}

const gchar *
gowl_config_get_border_color_unfocus(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self), "#313244");
	return self->border_hex_unfocus;
}

const gchar *
gowl_config_get_border_color_urgent(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self), "#f38ba8");
	return self->border_hex_urgent;
}

gboolean
gowl_config_get_animations(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self), FALSE);

	return self->animations;
}

gint
gowl_config_get_animation_duration(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self), 0);

	return self->animation_duration;
}

const gchar *
gowl_config_get_animation_curve(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self), NULL);

	return self->animation_curve;
}

gdouble
gowl_config_get_scroll_column_width(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self),
	                     GOWL_CONFIG_DEFAULT_SCROLL_COLUMN_WIDTH);

	return self->scroll_column_width;
}

gdouble
gowl_config_get_mfact(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self), GOWL_CONFIG_DEFAULT_MFACT);
	return self->mfact;
}

gint
gowl_config_get_nmaster(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self), GOWL_CONFIG_DEFAULT_NMASTER);
	return self->nmaster;
}

gint
gowl_config_get_tag_count(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self), GOWL_CONFIG_DEFAULT_TAG_COUNT);
	return self->tag_count;
}

gint
gowl_config_get_repeat_rate(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self), GOWL_CONFIG_DEFAULT_REPEAT_RATE);
	return self->repeat_rate;
}

gint
gowl_config_get_repeat_delay(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self), GOWL_CONFIG_DEFAULT_REPEAT_DELAY);
	return self->repeat_delay;
}

const gchar *
gowl_config_get_terminal(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self), GOWL_CONFIG_DEFAULT_TERMINAL);
	return self->terminal;
}

const gchar *
gowl_config_get_menu(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self), GOWL_CONFIG_DEFAULT_MENU);
	return self->menu;
}

gboolean
gowl_config_get_sloppyfocus(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self), GOWL_CONFIG_DEFAULT_SLOPPYFOCUS);
	return self->sloppyfocus;
}

gboolean
gowl_config_get_manage_lid(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self), GOWL_CONFIG_DEFAULT_MANAGE_LID);
	return self->manage_lid;
}

/**
 * gowl_config_get_idle_timeout:
 * @self: a #GowlConfig
 *
 * Returns: the idle timeout in seconds, 0 for never
 */
gint
gowl_config_get_idle_timeout(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self), GOWL_CONFIG_DEFAULT_IDLE_TIMEOUT);
	return self->idle_timeout;
}

/**
 * gowl_config_get_dpms_timeout:
 * @self: a #GowlConfig
 *
 * Returns: the output power-off timeout in seconds, 0 for never
 */
gint
gowl_config_get_dpms_timeout(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self), GOWL_CONFIG_DEFAULT_DPMS_TIMEOUT);
	return self->dpms_timeout;
}

/**
 * gowl_config_get_allow_tearing:
 * @self: a #GowlConfig
 *
 * Returns: %TRUE if fullscreen windows may tear when they ask to
 */
gboolean
gowl_config_get_allow_tearing(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self), GOWL_CONFIG_DEFAULT_ALLOW_TEARING);
	return self->allow_tearing;
}

/**
 * gowl_config_get_focus_on_activate:
 * @self: a #GowlConfig
 *
 * Returns: (transfer none): "smart", "urgent", "focus" or "none"
 */
const gchar *
gowl_config_get_focus_on_activate(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self), GOWL_CONFIG_DEFAULT_FOCUS_ON_ACTIVATE);
	return self->focus_on_activate != NULL
	       ? self->focus_on_activate : GOWL_CONFIG_DEFAULT_FOCUS_ON_ACTIVATE;
}

gboolean
gowl_config_get_input_recording(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self),
	                     GOWL_CONFIG_DEFAULT_INPUT_RECORDING);
	return self->input_recording;
}

const gchar *
gowl_config_get_input_recording_deny_apps(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self),
	                     GOWL_CONFIG_DEFAULT_INPUT_RECORDING_DENY_APPS);
	return self->input_recording_deny_apps;
}

const gchar *
gowl_config_get_log_level(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self), GOWL_CONFIG_DEFAULT_LOG_LEVEL);
	return self->log_level;
}

const gchar *
gowl_config_get_log_file(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self), GOWL_CONFIG_DEFAULT_LOG_FILE);
	return self->log_file;
}

/* --- cmacs evaluation gates --- */

gboolean
gowl_config_get_evaluate_gowl_config_with_cmacs(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self),
	                     GOWL_CONFIG_DEFAULT_EVALUATE_GOWL_CONFIG_WITH_CMACS);
	return self->evaluate_gowl_config_with_cmacs;
}

void
gowl_config_set_evaluate_gowl_config_with_cmacs(GowlConfig *self,
                                                 gboolean    value)
{
	g_return_if_fail(GOWL_IS_CONFIG(self));

	value = value ? TRUE : FALSE;
	if (self->evaluate_gowl_config_with_cmacs == value)
		return;

	self->evaluate_gowl_config_with_cmacs = value;
	g_object_notify_by_pspec(
		G_OBJECT(self),
		properties[GOWL_CONFIG_PROP_EVALUATE_GOWL_CONFIG_WITH_CMACS]);
	g_signal_emit(self, signals[SIGNAL_CHANGED], 0,
	              "evaluate-gowl-config-with-cmacs");
}

gboolean
gowl_config_get_evaluate_c_config_with_cmacs(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self),
	                     GOWL_CONFIG_DEFAULT_EVALUATE_C_CONFIG_WITH_CMACS);
	return self->evaluate_c_config_with_cmacs;
}

void
gowl_config_set_evaluate_c_config_with_cmacs(GowlConfig *self,
                                              gboolean    value)
{
	g_return_if_fail(GOWL_IS_CONFIG(self));

	value = value ? TRUE : FALSE;
	if (self->evaluate_c_config_with_cmacs == value)
		return;

	self->evaluate_c_config_with_cmacs = value;
	g_object_notify_by_pspec(
		G_OBJECT(self),
		properties[GOWL_CONFIG_PROP_EVALUATE_C_CONFIG_WITH_CMACS]);
	g_signal_emit(self, signals[SIGNAL_CHANGED], 0,
	              "evaluate-c-config-with-cmacs");
}

/**
 * gowl_config_reset_values_to_defaults:
 *
 * Restores every config property except the two cmacs evaluation
 * gates to its compile-time default.  Used by cmacs `--gowl` startup
 * when `evaluate-gowl-config-with-cmacs` is %FALSE: the YAML was
 * parsed fully (so notify:: fired for user intent) but the resulting
 * state is discarded except for the gates themselves.
 *
 * Also clears keybinds, rules, dropdowns, and module configs.  Emits
 * "reloaded" at the end so downstream listeners treat this like a
 * full reload.
 */
void
gowl_config_reset_values_to_defaults(GowlConfig *self)
{
	g_return_if_fail(GOWL_IS_CONFIG(self));

	g_object_freeze_notify(G_OBJECT(self));

	g_object_set(self,
	             "border-width",        GOWL_CONFIG_DEFAULT_BORDER_WIDTH,
	             "border-color-focus",  GOWL_CONFIG_DEFAULT_BORDER_COLOR_FOCUS,
	             "border-color-unfocus", GOWL_CONFIG_DEFAULT_BORDER_COLOR_UNFOCUS,
	             "border-color-urgent",  GOWL_CONFIG_DEFAULT_BORDER_COLOR_URGENT,
	             "mfact",               GOWL_CONFIG_DEFAULT_MFACT,
	             "nmaster",             GOWL_CONFIG_DEFAULT_NMASTER,
	             "tag-count",           GOWL_CONFIG_DEFAULT_TAG_COUNT,
	             "repeat-rate",         GOWL_CONFIG_DEFAULT_REPEAT_RATE,
	             "repeat-delay",        GOWL_CONFIG_DEFAULT_REPEAT_DELAY,
	             "terminal",            GOWL_CONFIG_DEFAULT_TERMINAL,
	             "menu",                GOWL_CONFIG_DEFAULT_MENU,
	             "sloppyfocus",         GOWL_CONFIG_DEFAULT_SLOPPYFOCUS,
	             "manage-lid",          GOWL_CONFIG_DEFAULT_MANAGE_LID,
	             "idle-timeout",        GOWL_CONFIG_DEFAULT_IDLE_TIMEOUT,
	             "dpms-timeout",        GOWL_CONFIG_DEFAULT_DPMS_TIMEOUT,
	             "allow-tearing",       GOWL_CONFIG_DEFAULT_ALLOW_TEARING,
	             "focus-on-activate",   GOWL_CONFIG_DEFAULT_FOCUS_ON_ACTIVATE,
	             "xkb-layout",          NULL,
	             "xkb-variant",         NULL,
	             "xkb-model",           NULL,
	             "xkb-options",         NULL,
	             "xkb-rules",           NULL,
	             "xkb-file",            NULL,
	             "input-recording",     GOWL_CONFIG_DEFAULT_INPUT_RECORDING,
	             "input-recording-deny-apps",
	                 GOWL_CONFIG_DEFAULT_INPUT_RECORDING_DENY_APPS,
	             "log-level",           GOWL_CONFIG_DEFAULT_LOG_LEVEL,
	             "log-file",            GOWL_CONFIG_DEFAULT_LOG_FILE,
	             NULL);

	if (self->keybinds != NULL)
		g_array_set_size(self->keybinds, 0);
	if (self->mousebinds != NULL) {
		g_array_set_size(self->mousebinds, 0);
		gowl_config_add_default_mousebinds(self);
	}
	if (self->gestures != NULL)
		g_array_set_size(self->gestures, 0);
	if (self->input_configs != NULL)
		g_ptr_array_set_size(self->input_configs, 0);
	if (self->rules != NULL)
		g_ptr_array_set_size(self->rules, 0);
	if (self->dropdowns != NULL)
		g_ptr_array_set_size(self->dropdowns, 0);
	if (self->module_configs != NULL)
		g_hash_table_remove_all(self->module_configs);
	if (self->monitor_configs != NULL)
		g_hash_table_remove_all(self->monitor_configs);
	g_list_free_full(self->profiles, output_profile_free);
	self->profiles = NULL;

	g_object_thaw_notify(G_OBJECT(self));

	g_signal_emit(self, signals[SIGNAL_RELOADED], 0);
}

/* --- Keybind management --- */

/**
 * gowl_config_add_keybind:
 * @self: a #GowlConfig
 * @modifiers: bitmask of #GowlKeyMod flags
 * @keysym: XKB keysym value
 * @action: a #GowlAction value
 * @arg: (nullable): optional argument string (will be copied)
 *
 * Appends a keybind entry with no description.  Thin wrapper over
 * gowl_config_add_keybind_full(); kept so that every pre-existing
 * caller compiles unchanged.
 */
void
gowl_config_add_keybind(
	GowlConfig  *self,
	guint        modifiers,
	guint        keysym,
	gint         action,
	const gchar *arg
){
	gowl_config_add_keybind_full(self, modifiers, keysym, action,
	                              arg, NULL);
}

/**
 * gowl_config_add_keybind_full:
 * @self: a #GowlConfig
 * @modifiers: bitmask of #GowlKeyMod flags
 * @keysym: XKB keysym value
 * @action: a #GowlAction value
 * @arg: (nullable): optional argument string (will be copied)
 * @desc: (nullable): human-readable description (will be copied)
 *
 * Appends a keybind entry to the internal keybind array.
 */
void
gowl_config_add_keybind_full(
	GowlConfig  *self,
	guint        modifiers,
	guint        keysym,
	gint         action,
	const gchar *arg,
	const gchar *desc
){
	gowl_config_add_keybind_ex(self, modifiers, keysym, action, arg, desc,
	                           NULL, GOWL_KEYBIND_FLAG_NONE);
}

void
gowl_config_add_keybind_ex(
	GowlConfig  *self,
	guint        modifiers,
	guint        keysym,
	gint         action,
	const gchar *arg,
	const gchar *desc,
	const gchar *mode,
	guint        flags
){
	GowlKeybindEntry entry;

	g_return_if_fail(GOWL_IS_CONFIG(self));

	entry.modifiers = modifiers;
	entry.keysym    = keysym;
	entry.action    = action;
	entry.arg       = g_strdup(arg);
	entry.desc      = g_strdup(desc);
	/* "default" and "" are the default mode, stored as NULL so that
	 * one comparison covers every way of writing it. */
	entry.mode      = (mode != NULL && *mode != '\0'
	                   && g_strcmp0(mode, "default") != 0)
	                  ? g_strdup(mode) : NULL;
	entry.flags     = flags;

	g_array_append_val(self->keybinds, entry);
}

/* --- Pointer binds --- */

void
gowl_config_add_mousebind(
	GowlConfig  *self,
	guint        modifiers,
	guint        button,
	gint         action,
	const gchar *arg,
	const gchar *desc
){
	GowlMousebindEntry entry;

	g_return_if_fail(GOWL_IS_CONFIG(self));

	gowl_config_remove_mousebind(self, modifiers, button);
	entry.modifiers = modifiers;
	entry.button    = button;
	entry.action    = action;
	entry.arg       = g_strdup(arg);
	entry.desc      = g_strdup(desc);
	g_array_append_val(self->mousebinds, entry);
}

guint
gowl_config_remove_mousebind(
	GowlConfig *self,
	guint       modifiers,
	guint       button
){
	guint i;
	guint removed = 0;

	g_return_val_if_fail(GOWL_IS_CONFIG(self), 0);

	for (i = 0; i < self->mousebinds->len; ) {
		GowlMousebindEntry *mb =
			&g_array_index(self->mousebinds, GowlMousebindEntry, i);

		if (mb->modifiers == modifiers && mb->button == button) {
			g_array_remove_index(self->mousebinds, i);
			removed++;
			continue;
		}
		i++;
	}
	return removed;
}

void
gowl_config_clear_mousebinds(GowlConfig *self)
{
	g_return_if_fail(GOWL_IS_CONFIG(self));
	g_array_set_size(self->mousebinds, 0);
}

GArray *
gowl_config_get_mousebinds(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self), NULL);
	return self->mousebinds;
}

/* --- Gesture binds --- */

void
gowl_config_add_gesture(
	GowlConfig  *self,
	gint         kind,
	gint         direction,
	guint        fingers,
	gint         action,
	const gchar *arg,
	const gchar *desc
){
	GowlGestureEntry entry;
	guint i;

	g_return_if_fail(GOWL_IS_CONFIG(self));

	for (i = 0; i < self->gestures->len; i++) {
		GowlGestureEntry *ge =
			&g_array_index(self->gestures, GowlGestureEntry, i);

		if (ge->kind == kind && ge->direction == direction
		    && ge->fingers == fingers) {
			g_array_remove_index(self->gestures, i);
			break;
		}
	}
	entry.kind      = kind;
	entry.direction = direction;
	entry.fingers   = fingers;
	entry.action    = action;
	entry.arg       = g_strdup(arg);
	entry.desc      = g_strdup(desc);
	g_array_append_val(self->gestures, entry);
}

void
gowl_config_clear_gestures(GowlConfig *self)
{
	g_return_if_fail(GOWL_IS_CONFIG(self));
	g_array_set_size(self->gestures, 0);
}

GArray *
gowl_config_get_gestures(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self), NULL);
	return self->gestures;
}

/* --- input: blocks --- */

void
gowl_config_add_input_setting(
	GowlConfig  *self,
	const gchar *match,
	const gchar *key,
	const gchar *value
){
	GowlInputConfigEntry *ic = NULL;
	guint i;

	g_return_if_fail(GOWL_IS_CONFIG(self));
	g_return_if_fail(match != NULL && key != NULL && value != NULL);

	for (i = 0; i < self->input_configs->len; i++) {
		GowlInputConfigEntry *e = g_ptr_array_index(self->input_configs, i);

		if (g_strcmp0(e->match, match) == 0) {
			ic = e;
			break;
		}
	}
	if (ic == NULL) {
		ic = g_new0(GowlInputConfigEntry, 1);
		ic->match = g_strdup(match);
		ic->settings = g_hash_table_new_full(g_str_hash, g_str_equal,
		                                     g_free, g_free);
		g_ptr_array_add(self->input_configs, ic);
	}
	g_hash_table_insert(ic->settings, g_strdup(key), g_strdup(value));
}

void
gowl_config_clear_input_settings(GowlConfig *self)
{
	g_return_if_fail(GOWL_IS_CONFIG(self));
	g_ptr_array_set_size(self->input_configs, 0);
}

GPtrArray *
gowl_config_get_input_configs(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self), NULL);
	return self->input_configs;
}

const gchar *
gowl_config_lookup_input_setting(
	GowlConfig  *self,
	const gchar *device_name,
	const gchar *device_class,
	const gchar *key
){
	const gchar *found = NULL;
	guint i;

	g_return_val_if_fail(GOWL_IS_CONFIG(self), NULL);
	g_return_val_if_fail(key != NULL, NULL);

	for (i = 0; i < self->input_configs->len; i++) {
		GowlInputConfigEntry *e = g_ptr_array_index(self->input_configs, i);
		const gchar *v;
		gboolean matches;

		if (g_strcmp0(e->match, "*") == 0)
			matches = TRUE;
		else if (device_class != NULL
		         && (g_ascii_strcasecmp(e->match, device_class) == 0
		             || (g_ascii_strcasecmp(e->match, "mouse") == 0
		                 && g_strcmp0(device_class, "pointer") == 0)))
			matches = TRUE;
		else if (device_name != NULL)
			matches = g_pattern_match_simple(e->match, device_name);
		else
			matches = FALSE;
		if (!matches)
			continue;
		v = g_hash_table_lookup(e->settings, key);
		if (v != NULL)
			found = v;
	}
	return found;
}

/* --- XKB --- */

const gchar *
gowl_config_get_xkb_layout(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self), NULL);
	return self->xkb_layout;
}

const gchar *
gowl_config_get_xkb_variant(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self), NULL);
	return self->xkb_variant;
}

const gchar *
gowl_config_get_xkb_model(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self), NULL);
	return self->xkb_model;
}

const gchar *
gowl_config_get_xkb_options(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self), NULL);
	return self->xkb_options;
}

const gchar *
gowl_config_get_xkb_rules(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self), NULL);
	return self->xkb_rules;
}

const gchar *
gowl_config_get_xkb_file(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self), NULL);
	return self->xkb_file;
}

/**
 * gowl_config_get_keybinds:
 * @self: a #GowlConfig
 *
 * Returns: (transfer none): the keybind GArray (element-type GowlKeybindEntry)
 */
GArray *
gowl_config_get_keybinds(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self), NULL);
	return self->keybinds;
}

/**
 * gowl_config_remove_keybind:
 * @self: a #GowlConfig
 * @modifiers: bitmask of #GowlKeyMod flags
 * @keysym: XKB keysym value
 *
 * Removes every keybind entry whose @modifiers and @keysym match the
 * arguments.  The @action and @arg fields are not compared, so all
 * binds registered for the same key combo are removed -- this is the
 * shape callers need to replace a stale bind with an authoritative one
 * (gowl's dispatch takes the first matching entry, so an older
 * duplicate would otherwise shadow a freshly-added one).  Returns the
 * number of entries removed.
 */
guint
gowl_config_remove_keybind(
	GowlConfig  *self,
	guint        modifiers,
	guint        keysym
){
	guint i;
	guint removed = 0;

	g_return_val_if_fail(GOWL_IS_CONFIG(self), 0);

	for (i = 0; i < self->keybinds->len; ) {
		GowlKeybindEntry *kb;

		kb = &g_array_index(self->keybinds, GowlKeybindEntry, i);
		if (kb->modifiers == modifiers && kb->keysym == keysym) {
			/* Order-preserving remove: shifts the tail down so the
			 * next candidate slides into slot i -- do not advance. */
			g_array_remove_index(self->keybinds, i);
			removed++;
			continue;
		}
		i++;
	}

	return removed;
}

/**
 * gowl_config_clear_keybinds:
 * @self: a #GowlConfig
 *
 * Removes every keybind from the config.  The per-entry clear func
 * frees each entry's arg string.
 */
void
gowl_config_clear_keybinds(GowlConfig *self)
{
	g_return_if_fail(GOWL_IS_CONFIG(self));

	if (self->keybinds != NULL)
		g_array_set_size(self->keybinds, 0);
}

/* --- Rule management --- */

/**
 * gowl_config_add_rule:
 * @self: a #GowlConfig
 * @app_id: (nullable): app_id pattern or %NULL
 * @title: (nullable): title pattern or %NULL
 * @tags: tag bitmask
 * @floating: whether to float
 * @monitor: target monitor index or -1
 *
 * Allocates a new #GowlRuleEntry, copies the strings, and appends
 * it to the rules array.
 */
void
gowl_config_add_rule(
	GowlConfig  *self,
	const gchar *app_id,
	const gchar *title,
	guint32      tags,
	gboolean     floating,
	gint         monitor
){
	gowl_config_add_rule_full(self, app_id, title, tags, floating,
	                           monitor, 0, 0, TRUE, FALSE);
}

/**
 * gowl_config_add_rule_full:
 * @self: a #GowlConfig
 * @app_id: (nullable): app_id pattern or %NULL
 * @title: (nullable): title pattern or %NULL
 * @tags: tag bitmask
 * @floating: whether to float
 * @monitor: target monitor index or -1
 * @width: explicit width in pixels, or 0 for natural
 * @height: explicit height in pixels, or 0 for natural
 * @center: center on monitor when floating
 * @regex_mode: interpret patterns as PCRE regexes
 *
 * Allocates a new #GowlRuleEntry with every tunable field and
 * appends it to the rules array.  Called by gowl_config_add_rule()
 * with sensible defaults for the v2 fields.
 */
void
gowl_config_add_rule_full(
	GowlConfig  *self,
	const gchar *app_id,
	const gchar *title,
	guint32      tags,
	gboolean     floating,
	gint         monitor,
	gint         width,
	gint         height,
	gboolean     center,
	gboolean     regex_mode
){
	GowlRuleEntry *rule;

	g_return_if_fail(GOWL_IS_CONFIG(self));

	rule = g_new0(GowlRuleEntry, 1);
	rule->app_id     = g_strdup(app_id);
	rule->title      = g_strdup(title);
	rule->tags       = tags;
	rule->floating   = floating;
	rule->monitor    = monitor;
	rule->width      = width;
	rule->height     = height;
	rule->center     = center;
	rule->regex_mode = regex_mode;
	rule->sticky     = FALSE;
	/* The tri-states: -1 is "any", and 0 is a real value for each of
	 * them, so they cannot be left zeroed.  Same trap as
	 * gowl_rule_entry_init guards for callers building an entry. */
	rule->xwayland         = -1;
	rule->match_floating   = -1;
	rule->match_fullscreen = -1;

	g_ptr_array_add(self->rules, rule);
}

/**
 * gowl_rule_entry_init:
 * @entry: a #GowlRuleEntry to prepare
 *
 * Zeroes @entry and writes its non-zero defaults; see the header.
 */
void
gowl_rule_entry_init(GowlRuleEntry *entry)
{
	g_return_if_fail(entry != NULL);

	memset(entry, 0, sizeof *entry);
	entry->monitor = -1;
	entry->center = TRUE;
	entry->xwayland = -1;
	entry->match_floating = -1;
	entry->match_fullscreen = -1;
}

void
gowl_config_add_rule_entry(
	GowlConfig          *self,
	const GowlRuleEntry *entry
){
	GowlRuleEntry *rule;

	g_return_if_fail(GOWL_IS_CONFIG(self));
	g_return_if_fail(entry != NULL);

	rule = g_new0(GowlRuleEntry, 1);
	*rule = *entry;
	rule->app_id = g_strdup(entry->app_id);
	rule->title  = g_strdup(entry->title);
	rule->initial_title = g_strdup(entry->initial_title);
	g_ptr_array_add(self->rules, rule);
}

/**
 * gowl_config_remove_rule:
 * @self: a #GowlConfig
 * @app_id: (nullable): app_id pattern to match the rule by
 * @title: (nullable): title pattern to match the rule by
 *
 * Removes the first rule entry whose @app_id and @title strings
 * match the arguments verbatim.  Comparison is by literal string;
 * %NULL matches %NULL, non-%NULL uses g_strcmp0().  Returns the
 * number of rules removed.
 */
guint
gowl_config_remove_rule(
	GowlConfig  *self,
	const gchar *app_id,
	const gchar *title
){
	guint i;

	g_return_val_if_fail(GOWL_IS_CONFIG(self), 0);

	for (i = 0; i < self->rules->len; i++) {
		GowlRuleEntry *rule;

		rule = (GowlRuleEntry *)g_ptr_array_index(self->rules, i);
		if (g_strcmp0(rule->app_id, app_id) == 0 &&
		    g_strcmp0(rule->title, title) == 0) {
			g_ptr_array_remove_index(self->rules, i);
			return 1;
		}
	}

	return 0;
}

/**
 * gowl_config_clear_rules:
 * @self: a #GowlConfig
 *
 * Removes every rule from the config.
 */
void
gowl_config_clear_rules(GowlConfig *self)
{
	g_return_if_fail(GOWL_IS_CONFIG(self));

	if (self->rules->len > 0)
		g_ptr_array_remove_range(self->rules, 0, self->rules->len);
}

/* --- Dropdown management --- */

/**
 * gowl_config_add_dropdown:
 *
 * Allocates a new #GowlDropdownEntry, copies the strings, and
 * appends it to the dropdowns array.  Adding an entry with a
 * duplicate @name silently replaces the existing entry.
 */
void
gowl_config_add_dropdown(
	GowlConfig  *self,
	const gchar *name,
	const gchar *spawn_cmd,
	const gchar *keybind,
	gdouble      width_pct,
	gdouble      height_pct,
	gint         width_abs,
	gint         height_abs,
	gint         anchor
){
	GowlDropdownEntry *dd;

	g_return_if_fail(GOWL_IS_CONFIG(self));
	g_return_if_fail(name != NULL);
	g_return_if_fail(spawn_cmd != NULL);

	gowl_config_remove_dropdown(self, name);

	dd = g_new0(GowlDropdownEntry, 1);
	dd->name       = g_strdup(name);
	dd->spawn_cmd  = g_strdup(spawn_cmd);
	dd->keybind    = g_strdup(keybind);
	dd->width_pct  = width_pct;
	dd->height_pct = height_pct;
	dd->width_abs  = width_abs;
	dd->height_abs = height_abs;
	dd->anchor     = anchor;

	g_ptr_array_add(self->dropdowns, dd);
}

/**
 * gowl_config_remove_dropdown:
 *
 * Removes the dropdown entry whose @name matches exactly.
 * Returns 1 on removal, 0 if nothing matched.
 */
guint
gowl_config_remove_dropdown(
	GowlConfig  *self,
	const gchar *name
){
	guint i;

	g_return_val_if_fail(GOWL_IS_CONFIG(self), 0);
	g_return_val_if_fail(name != NULL, 0);

	for (i = 0; i < self->dropdowns->len; i++) {
		GowlDropdownEntry *dd;

		dd = (GowlDropdownEntry *)g_ptr_array_index(self->dropdowns, i);
		if (g_strcmp0(dd->name, name) == 0) {
			g_ptr_array_remove_index(self->dropdowns, i);
			return 1;
		}
	}

	return 0;
}

/**
 * gowl_config_get_dropdowns:
 *
 * Returns the #GPtrArray backing the dropdown entries.  The
 * array is borrowed; the caller must not free it.
 */
GPtrArray *
gowl_config_get_dropdowns(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self), NULL);
	return self->dropdowns;
}

/**
 * gowl_config_get_rules:
 * @self: a #GowlConfig
 *
 * Returns: (transfer none): the rules GPtrArray (element-type GowlRuleEntry)
 */
GPtrArray *
gowl_config_get_rules(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self), NULL);
	return self->rules;
}

/**
 * gowl_config_get_module_config:
 * @self: a #GowlConfig
 * @module_name: the name of the module (e.g. "vanitygaps")
 *
 * Returns the per-module settings parsed from the YAML config's
 * `modules:` section.  The returned hash table maps setting keys
 * (e.g. "inner-h") to string values (e.g. "10").  Callers must
 * convert to the appropriate type.
 *
 * Returns: (transfer none) (nullable): a #GHashTable of string
 *          key-value pairs, or %NULL if no config for @module_name
 */
GHashTable *
gowl_config_get_module_config(
	GowlConfig  *self,
	const gchar *module_name
){
	g_return_val_if_fail(GOWL_IS_CONFIG(self), NULL);
	g_return_val_if_fail(module_name != NULL, NULL);

	return (GHashTable *)g_hash_table_lookup(
		self->module_configs, module_name);
}

/**
 * gowl_config_get_all_module_configs:
 * @self: a #GowlConfig
 *
 * Returns the entire module configuration table.  The outer hash
 * maps module names (strings) to inner #GHashTable objects of
 * string key-value settings.
 *
 * Returns: (transfer none) (nullable): the module configs hash table
 */
GHashTable *
gowl_config_get_all_module_configs(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self), NULL);
	return self->module_configs;
}

/**
 * gowl_config_get_monitor_config:
 * @self: a #GowlConfig
 * @name: the output name (e.g. "eDP-1")
 *
 * Looks up the per-output config parsed from `monitors:`.
 *
 * Returns: (transfer none) (nullable): #GowlMonitorConfig owned by
 *          @self, or %NULL if no entry exists for @name
 */
const GowlMonitorConfig *
gowl_config_get_monitor_config(
	GowlConfig  *self,
	const gchar *name
){
	g_return_val_if_fail(GOWL_IS_CONFIG(self), NULL);
	g_return_val_if_fail(name != NULL, NULL);

	return (const GowlMonitorConfig *)g_hash_table_lookup(
		self->monitor_configs, name);
}

/**
 * gowl_config_get_monitor_names:
 * @self: a #GowlConfig
 *
 * Lists every output name that has an entry in the parsed
 * `monitors:` mapping.  The list itself is owned by the caller
 * (g_list_free), but the string elements are borrowed from
 * @self's internal hash and must not be freed.
 *
 * Returns: (transfer container) (element-type utf8): a #GList
 */
GList *
gowl_config_get_monitor_names(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self), NULL);
	return g_hash_table_get_keys(self->monitor_configs);
}

/**
 * gowl_monitor_config_init:
 * @mc: a #GowlMonitorConfig to prepare
 *
 * Zeroes @mc and writes the "unset" sentinels; see the header.
 */
void
gowl_monitor_config_init(GowlMonitorConfig *mc)
{
	g_return_if_fail(mc != NULL);

	memset(mc, 0, sizeof *mc);
	mc->x = G_MININT;
	mc->y = G_MININT;
	mc->transform = -1;
	mc->enabled = -1;
	mc->vrr = -1;
	mc->hdr = -1;
}

/**
 * gowl_config_set_monitor_config:
 * @self: a #GowlConfig
 * @key: an output key
 * @mc: (nullable): the configuration, copied; %NULL removes the entry
 *
 * Sets one `monitors:` entry from code rather than from YAML.
 */
void
gowl_config_set_monitor_config(
	GowlConfig              *self,
	const gchar             *key,
	const GowlMonitorConfig *mc
){
	GowlMonitorConfig *copy;

	g_return_if_fail(GOWL_IS_CONFIG(self));
	g_return_if_fail(key != NULL);

	if (mc == NULL) {
		g_hash_table_remove(self->monitor_configs, key);
		return;
	}
	copy = g_new0(GowlMonitorConfig, 1);
	*copy = *mc;
	g_hash_table_insert(self->monitor_configs, g_strdup(key), copy);
}

/**
 * gowl_config_add_output_profile:
 * @self: a #GowlConfig
 * @name: the profile's name
 *
 * Returns: (transfer none): the named profile, created empty at the end
 *          of the list if it did not exist
 */
GowlOutputProfile *
gowl_config_add_output_profile(GowlConfig *self, const gchar *name)
{
	GowlOutputProfile *profile;
	GList *l;

	g_return_val_if_fail(GOWL_IS_CONFIG(self), NULL);
	g_return_val_if_fail(name != NULL, NULL);

	for (l = self->profiles; l != NULL; l = l->next) {
		profile = (GowlOutputProfile *)l->data;
		if (g_strcmp0(profile->name, name) == 0)
			return profile;
	}
	profile = g_new0(GowlOutputProfile, 1);
	profile->name = g_strdup(name);
	profile->outputs = g_hash_table_new_full(g_str_hash, g_str_equal,
	                                         g_free, g_free);
	self->profiles = g_list_append(self->profiles, profile);
	return profile;
}

/**
 * gowl_output_profile_set_output:
 * @profile: a #GowlOutputProfile
 * @key: an output key
 * @mc: (nullable): what the output gets, copied; %NULL for "present,
 *      settings from `monitors:`"
 *
 * Adds or replaces one output of @profile.
 */
void
gowl_output_profile_set_output(
	GowlOutputProfile       *profile,
	const gchar             *key,
	const GowlMonitorConfig *mc
){
	GowlMonitorConfig *copy;

	g_return_if_fail(profile != NULL);
	g_return_if_fail(key != NULL);

	copy = g_new0(GowlMonitorConfig, 1);
	if (mc != NULL)
		*copy = *mc;
	else
		gowl_monitor_config_init(copy);
	g_hash_table_insert(profile->outputs, g_strdup(key), copy);
}

/**
 * gowl_config_remove_output_profile:
 * @self: a #GowlConfig
 * @name: the profile to remove
 *
 * Returns: %TRUE if one was removed
 */
gboolean
gowl_config_remove_output_profile(GowlConfig *self, const gchar *name)
{
	GList *l;

	g_return_val_if_fail(GOWL_IS_CONFIG(self), FALSE);
	g_return_val_if_fail(name != NULL, FALSE);

	for (l = self->profiles; l != NULL; l = l->next) {
		GowlOutputProfile *profile = (GowlOutputProfile *)l->data;

		if (g_strcmp0(profile->name, name) != 0)
			continue;
		self->profiles = g_list_remove_link(self->profiles, l);
		output_profile_free(profile);
		g_list_free_1(l);
		return TRUE;
	}
	return FALSE;
}

/**
 * gowl_config_get_output_profiles:
 * @self: a #GowlConfig
 *
 * Returns: (transfer none) (element-type GowlOutputProfile): the
 *          `profiles:` section in file order
 */
GList *
gowl_config_get_output_profiles(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self), NULL);
	return self->profiles;
}

/**
 * gowl_config_output_key_matches:
 * @key: a key from `monitors:` or a profile's outputs
 * @name: the connector name of an output
 * @make: (nullable): its make
 * @model: (nullable): its model
 * @serial: (nullable): its serial
 *
 * Whether @key names this output; see the header.
 *
 * Returns: %TRUE if it matches
 */
gboolean
gowl_config_output_key_matches(
	const gchar *key,
	const gchar *name,
	const gchar *make,
	const gchar *model,
	const gchar *serial
){
	g_autofree gchar *mms = NULL;
	g_autofree gchar *mm = NULL;

	if (key == NULL)
		return FALSE;
	if (g_strcmp0(key, "*") == 0)
		return TRUE;
	if (name != NULL && g_ascii_strcasecmp(key, name) == 0)
		return TRUE;
	if (make == NULL || model == NULL)
		return FALSE;
	mm = g_strdup_printf("%s %s", make, model);
	if (g_ascii_strcasecmp(key, mm) == 0)
		return TRUE;
	if (serial == NULL)
		return FALSE;
	mms = g_strdup_printf("%s %s %s", make, model, serial);
	return g_ascii_strcasecmp(key, mms) == 0;
}

/**
 * lookup_key_in:
 * @table: output key -> anything
 *
 * The value whose key matches the output.  A connector name or a full
 * description wins over a "Make Model" key, which wins over "*".
 *
 * Untyped because two tables are keyed this way -- `monitors:` by
 * #GowlMonitorConfig and `wallpaper-outputs:` by #GowlWallpaperOutput --
 * and the precedence between an exact connector name, a serial, a
 * make/model and the wildcard is the same rule for both.
 */
static gpointer
lookup_key_in(
	GHashTable  *table,
	const gchar *name,
	const gchar *make,
	const gchar *model,
	const gchar *serial
){
	GHashTableIter iter;
	gpointer k;
	gpointer v;
	gpointer wildcard = NULL;
	gpointer by_mm = NULL;

	if (table == NULL)
		return NULL;
	g_hash_table_iter_init(&iter, table);
	while (g_hash_table_iter_next(&iter, &k, &v)) {
		const gchar *key = (const gchar *)k;

		if (!gowl_config_output_key_matches(key, name, make, model, serial))
			continue;
		if (g_strcmp0(key, "*") == 0)
			wildcard = v;
		else if (name != NULL && g_ascii_strcasecmp(key, name) == 0)
			return v;
		else if (serial != NULL && g_str_has_suffix(key, serial))
			return v;
		else
			by_mm = v;
	}
	return by_mm != NULL ? by_mm : wildcard;
}

static const GowlMonitorConfig *
lookup_by_key(
	GHashTable  *table,
	const gchar *name,
	const gchar *make,
	const gchar *model,
	const gchar *serial
){
	return (const GowlMonitorConfig *)lookup_key_in(table, name, make,
	                                                model, serial);
}

/**
 * gowl_config_lookup_monitor_config:
 * @self: a #GowlConfig
 * @profile: (nullable): the output profile in force
 * @name: the connector name of an output
 * @make: (nullable): its make
 * @model: (nullable): its model
 * @serial: (nullable): its serial
 *
 * Returns: (transfer none) (nullable): the profile's entry for the
 *          output, else the `monitors:` one
 */
const GowlMonitorConfig *
gowl_config_lookup_monitor_config(
	GowlConfig              *self,
	const GowlOutputProfile *profile,
	const gchar             *name,
	const gchar             *make,
	const gchar             *model,
	const gchar             *serial
){
	const GowlMonitorConfig *mc = NULL;

	g_return_val_if_fail(GOWL_IS_CONFIG(self), NULL);
	if (profile != NULL)
		mc = lookup_by_key(profile->outputs, name, make, model, serial);
	if (mc == NULL)
		mc = lookup_by_key(self->monitor_configs, name, make, model, serial);
	return mc;
}

/* ── Palette ─────────────────────────────────────────────────────── */

/**
 * gowl_config_get_palette:
 * @self: a #GowlConfig
 *
 * The effective palette: the named built-in, with the config file's
 * `palette:' entries and then any runtime overrides layered on it.
 *
 * Returns: (transfer none): the palette.  Never %NULL.
 */
GowlPalette *
gowl_config_get_palette(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self), NULL);
	return self->palette;
}

/**
 * gowl_config_get_palette_name:
 * @self: a #GowlConfig
 *
 * Returns: (transfer none): the built-in flavour the palette starts from.
 */
const gchar *
gowl_config_get_palette_name(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self), NULL);
	return self->palette_name;
}

/**
 * gowl_config_set_palette_name:
 * @self: a #GowlConfig
 * @name: (nullable): a built-in palette name
 *
 * Switches the flavour the palette starts from, keeping every override.
 * Everything that reads a colour through the config picks the change up
 * on the next arrange; nothing is repainted here.
 */
void
gowl_config_set_palette_name(GowlConfig *self, const gchar *name)
{
	g_return_if_fail(GOWL_IS_CONFIG(self));

	g_free(self->palette_name);
	self->palette_name = g_strdup(name != NULL
	                              ? name : GOWL_CONFIG_DEFAULT_PALETTE);

	/* No from_file layer: the file's entries were folded into the
	 * override-free base at load time and are re-read on reload.  A
	 * flavour switch between reloads keeps only the runtime
	 * overrides, which is the layer the caller owns. */
	gowl_config_rebuild_palette(self, NULL);
}

/**
 * gowl_config_set_palette_color:
 * @self: a #GowlConfig
 * @name: a palette entry name
 * @hex: (nullable): a literal colour, or %NULL to drop the override
 *
 * Overrides one palette entry at runtime.  Overrides sit above the
 * config file, so they survive a reload --- which is what makes
 * "follow the editor's theme" work: the theme pushes its colours in
 * once and a later `gowl-reload-config' does not undo it.
 */
void
gowl_config_set_palette_color(GowlConfig  *self,
                              const gchar *name,
                              const gchar *hex)
{
	g_return_if_fail(GOWL_IS_CONFIG(self));
	g_return_if_fail(name != NULL);

	gowl_palette_set(self->palette_override, name, hex);

	/* Rebuilding from the built-in drops the file's entries until the
	 * next reload.  Setting the entry on the effective palette too
	 * keeps them, at the cost of the override being invisible in
	 * `palette_override' order --- which nothing depends on. */
	gowl_palette_set(self->palette, name, hex);
	gowl_config_reresolve_colors(self);
}

/**
 * gowl_config_resolve_color:
 * @self: a #GowlConfig
 * @spec: (nullable): a colour spec --- a literal, a palette entry name,
 *   or `name/aa'
 *
 * Resolves a colour spec against the config's palette.  Public so that
 * a module handling colours outside the `*color*' setting convention,
 * or building one at runtime, can still go through the palette.
 *
 * Returns: (transfer full) (nullable): a newly allocated hex string.
 */
gchar *
gowl_config_resolve_color(GowlConfig *self, const gchar *spec)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self), g_strdup(spec));
	return gowl_palette_resolve(self->palette, spec);
}

/**
 * gowl_config_get_animation_duration_open:
 * @self: a #GowlConfig
 *
 * How long a window's open animation runs, in milliseconds.
 *
 * Returns: the duration, or -1 to mean "use the general
 *   `animation-duration'".
 */
gint
gowl_config_get_animation_duration_open(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self), -1);
	return self->animation_duration_open;
}

/**
 * gowl_config_get_animation_duration_close:
 * @self: a #GowlConfig
 *
 * How long a window's close animation runs, in milliseconds.
 *
 * Returns: the duration, or -1 to mean "use the general
 *   `animation-duration'".
 */
gint
gowl_config_get_animation_duration_close(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self), -1);
	return self->animation_duration_close;
}

const gchar *
gowl_config_get_animation_curve_open(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self), GOWL_CONFIG_DEFAULT_ANIMATION_CURVE_OPEN);
	return self->animation_curve_open;
}

const gchar *
gowl_config_get_animation_curve_close(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self), GOWL_CONFIG_DEFAULT_ANIMATION_CURVE_CLOSE);
	return self->animation_curve_close != NULL
	       ? self->animation_curve_close
	       : GOWL_CONFIG_DEFAULT_ANIMATION_CURVE_CLOSE;
}

gdouble
gowl_config_get_animation_popin_scale(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self), GOWL_CONFIG_DEFAULT_ANIMATION_POPIN_SCALE);
	return self->animation_popin_scale;
}

gdouble
gowl_config_get_animation_jiggle_strength(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self), 1.0);
	return self->animation_jiggle_strength;
}

/* --- Desktop cube --- */

gboolean
gowl_config_get_cube(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self), FALSE);
	return self->cube;
}

gint
gowl_config_get_cube_duration(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self),
	                     GOWL_CONFIG_DEFAULT_CUBE_DURATION);
	return self->cube_duration;
}

gint
gowl_config_get_cube_step_duration(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self),
	                     GOWL_CONFIG_DEFAULT_CUBE_STEP_DURATION);
	return self->cube_step_duration;
}

const gchar *
gowl_config_get_cube_curve(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self),
	                     GOWL_CONFIG_DEFAULT_CUBE_CURVE);
	return self->cube_curve;
}

gint
gowl_config_get_cube_faces(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self),
	                     GOWL_CONFIG_DEFAULT_CUBE_FACES);
	return self->cube_faces;
}

gdouble
gowl_config_get_cube_zoom(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self),
	                     GOWL_CONFIG_DEFAULT_CUBE_ZOOM);
	return self->cube_zoom;
}

gdouble
gowl_config_get_cube_pitch(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self),
	                     GOWL_CONFIG_DEFAULT_CUBE_PITCH);
	return self->cube_pitch;
}

gdouble
gowl_config_get_cube_shading(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self),
	                     GOWL_CONFIG_DEFAULT_CUBE_SHADING);
	return self->cube_shading;
}

gdouble
gowl_config_get_cube_reflection(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self),
	                     GOWL_CONFIG_DEFAULT_CUBE_REFLECTION);
	return self->cube_reflection;
}

gdouble
gowl_config_get_cube_motion_blur(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self),
	                     GOWL_CONFIG_DEFAULT_CUBE_MOTION_BLUR);
	return self->cube_motion_blur;
}

const gchar *
gowl_config_get_cube_backdrop_color(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self),
	                     GOWL_CONFIG_DEFAULT_CUBE_BACKDROP_COLOR);
	return self->cube_backdrop_color;
}

gboolean
gowl_config_get_cube_caps(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self), TRUE);
	return self->cube_caps;
}

gboolean
gowl_config_get_cube_all_monitors(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self), FALSE);
	return self->cube_all_monitors;
}

gboolean
gowl_config_get_cube_gesture(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self), FALSE);
	return self->cube_gesture;
}

/* --- Magnifier --- */

gboolean
gowl_config_get_magnifier(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self), FALSE);
	return self->magnifier;
}

gdouble
gowl_config_get_magnifier_max(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self),
	                     GOWL_CONFIG_DEFAULT_MAGNIFIER_MAX);
	return self->magnifier_max;
}

gdouble
gowl_config_get_magnifier_step(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self),
	                     GOWL_CONFIG_DEFAULT_MAGNIFIER_STEP);
	return self->magnifier_step;
}

gint
gowl_config_get_magnifier_smoothing(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self),
	                     GOWL_CONFIG_DEFAULT_MAGNIFIER_SMOOTHING);
	return self->magnifier_smoothing;
}

gboolean
gowl_config_get_magnifier_follow_cursor(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self), TRUE);
	return self->magnifier_follow_cursor;
}

gboolean
gowl_config_get_magnifier_smooth(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self), TRUE);
	return self->magnifier_smooth;
}

const gchar *
gowl_config_get_magnifier_modifier(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self),
	                     GOWL_CONFIG_DEFAULT_MAGNIFIER_MODIFIER);
	return self->magnifier_modifier;
}

/* --- Expo --- */

gboolean
gowl_config_get_expo(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self), FALSE);
	return self->expo;
}

gint
gowl_config_get_expo_duration(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self),
	                     GOWL_CONFIG_DEFAULT_EXPO_DURATION);
	return self->expo_duration;
}

const gchar *
gowl_config_get_expo_curve(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self),
	                     GOWL_CONFIG_DEFAULT_EXPO_CURVE);
	return self->expo_curve;
}

gint
gowl_config_get_expo_tags(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self),
	                     GOWL_CONFIG_DEFAULT_EXPO_TAGS);
	return self->expo_tags;
}

gint
gowl_config_get_expo_columns(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self), 0);
	return self->expo_columns;
}

gdouble
gowl_config_get_expo_gap(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self), GOWL_CONFIG_DEFAULT_EXPO_GAP);
	return self->expo_gap;
}

gdouble
gowl_config_get_expo_corner(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self), GOWL_CONFIG_DEFAULT_EXPO_CORNER);
	return self->expo_corner;
}

gdouble
gowl_config_get_expo_dim(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self), GOWL_CONFIG_DEFAULT_EXPO_DIM);
	return self->expo_dim;
}

gboolean
gowl_config_get_expo_hide_empty(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self), FALSE);
	return self->expo_hide_empty;
}

const gchar *
gowl_config_get_expo_backdrop_color(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self),
	                     GOWL_CONFIG_DEFAULT_EXPO_BACKDROP_COLOR);
	return self->expo_backdrop_color;
}

/* --- Switcher --- */

gboolean
gowl_config_get_switcher(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self), FALSE);
	return self->switcher;
}

gint
gowl_config_get_switcher_duration(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self),
	                     GOWL_CONFIG_DEFAULT_SWITCHER_DURATION);
	return self->switcher_duration;
}

const gchar *
gowl_config_get_switcher_curve(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self),
	                     GOWL_CONFIG_DEFAULT_SWITCHER_CURVE);
	return self->switcher_curve;
}

gdouble
gowl_config_get_switcher_scale(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self),
	                     GOWL_CONFIG_DEFAULT_SWITCHER_SCALE);
	return self->switcher_scale;
}

gdouble
gowl_config_get_switcher_spacing(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self),
	                     GOWL_CONFIG_DEFAULT_SWITCHER_SPACING);
	return self->switcher_spacing;
}

gdouble
gowl_config_get_switcher_angle(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self),
	                     GOWL_CONFIG_DEFAULT_SWITCHER_ANGLE);
	return self->switcher_angle;
}

gdouble
gowl_config_get_switcher_reflection(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self),
	                     GOWL_CONFIG_DEFAULT_SWITCHER_REFLECTION);
	return self->switcher_reflection;
}

gboolean
gowl_config_get_switcher_all_tags(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self), FALSE);
	return self->switcher_all_tags;
}

const gchar *
gowl_config_get_switcher_backdrop_color(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self),
	                     GOWL_CONFIG_DEFAULT_SWITCHER_BACKDROP_COLOR);
	return self->switcher_backdrop_color;
}

/* --- Blur and shadows --- */

gboolean
gowl_config_get_blur(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self), FALSE);
	return self->blur;
}

gint
gowl_config_get_blur_downscale(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self),
	                     GOWL_CONFIG_DEFAULT_BLUR_DOWNSCALE);
	return self->blur_downscale;
}

gint
gowl_config_get_blur_passes(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self),
	                     GOWL_CONFIG_DEFAULT_BLUR_PASSES);
	return self->blur_passes;
}

gdouble
gowl_config_get_blur_brightness(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self),
	                     GOWL_CONFIG_DEFAULT_BLUR_BRIGHTNESS);
	return self->blur_brightness;
}

/* --- What shows through a translucent window, and the glass --- */

GowlBackdropStyle
gowl_config_get_backdrop_style(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self), GOWL_BACKDROP_GLASS);
	return (GowlBackdropStyle)self->backdrop_style;
}

void
gowl_config_set_backdrop_style(GowlConfig *self, GowlBackdropStyle style)
{
	g_return_if_fail(GOWL_IS_CONFIG(self));

	/*
	 * Asked of the ENUM rather than compared against its last member.
	 *
	 * This used to read `style > GOWL_BACKDROP_WATER', which was correct
	 * exactly until a fifth backdrop was added -- and then silently
	 * refused it, so the cycle key stopped on the new one and the toast
	 * announced a style the config had just declined to store.  A bound
	 * written as a member name is a bound that has to be remembered.
	 */
	if (g_enum_get_value(backdrop_enum_class(), (gint)style) == NULL)
		return;
	self->backdrop_style = (gint)style;
}

/*
 * The names come from the enum's own nicks rather than a second table
 * here, so the config spelling, the IPC reply and the Lisp symbol cannot
 * drift apart from what gowl-enums.c registered.
 *
 * The class reference is taken ONCE and never dropped, deliberately.  The
 * nick is a static string the class owns, and handing it out only to
 * release the class in the same breath would be relying on GLib never
 * finalising a static enum class -- true today, and not a thing to make
 * this depend on.  One permanent reference to one enum class costs
 * nothing and settles it.
 */
static GEnumClass *
backdrop_enum_class(void)
{
	static gsize once = 0;

	if (g_once_init_enter(&once)) {
		GEnumClass *ec = (GEnumClass *)g_type_class_ref(
			gowl_backdrop_style_get_type());

		g_once_init_leave(&once, (gsize)ec);
	}
	return (GEnumClass *)once;
}

const gchar *
gowl_config_backdrop_style_name(GowlBackdropStyle style)
{
	GEnumValue *ev = g_enum_get_value(backdrop_enum_class(), (gint)style);

	return ev != NULL && ev->value_nick != NULL ? ev->value_nick : "glass";
}

gboolean
gowl_config_backdrop_style_from_name(const gchar       *name,
                                     GowlBackdropStyle *out)
{
	GEnumValue       *ev;
	g_autofree gchar *norm = NULL;

	if (name == NULL || out == NULL)
		return FALSE;

	norm = g_strstrip(g_ascii_strdown(name, -1));
	g_strdelimit(norm, "_", '-');
	ev = g_enum_get_value_by_nick(backdrop_enum_class(), norm);
	if (ev != NULL) {
		*out = (GowlBackdropStyle)ev->value;
		return TRUE;
	}

	/*
	 * The names a person would reach for that are not the nicks.
	 *
	 * The nick is the short form because it is also the prefix on ninety
	 * config keys -- `fizz-site-width' rather than
	 * `carbonation-site-width' -- but nobody asked for a "fizz"
	 * backdrop, they asked for carbonation, and the toast says
	 * "Carbonation".  A setting whose displayed name is not accepted as
	 * its own value is a small trap laid for whoever reads the toast and
	 * types it.
	 */
	if (g_strcmp0(norm, "carbonation") == 0 || g_strcmp0(norm, "soda") == 0
	    || g_strcmp0(norm, "bubbles") == 0) {
		*out = GOWL_BACKDROP_FIZZ;
		return TRUE;
	}
	if (g_strcmp0(norm, "leaf") == 0 || g_strcmp0(norm, "autumn") == 0
	    || g_strcmp0(norm, "fall") == 0) {
		*out = GOWL_BACKDROP_LEAVES;
		return TRUE;
	}
	if (g_strcmp0(norm, "snowfall") == 0) {
		*out = GOWL_BACKDROP_SNOW;
		return TRUE;
	}
	if (g_strcmp0(norm, "thunderstorm") == 0
	    || g_strcmp0(norm, "thunder") == 0
	    || g_strcmp0(norm, "lightning") == 0) {
		*out = GOWL_BACKDROP_STORM;
		return TRUE;
	}
	if (g_strcmp0(norm, "soap-film") == 0 || g_strcmp0(norm, "film") == 0
	    || g_strcmp0(norm, "iridescence") == 0) {
		*out = GOWL_BACKDROP_SOAP;
		return TRUE;
	}
	if (g_strcmp0(norm, "underwater") == 0 || g_strcmp0(norm, "deep") == 0
	    || g_strcmp0(norm, "ocean") == 0) {
		*out = GOWL_BACKDROP_SUBMERGED;
		return TRUE;
	}
	if (g_strcmp0(norm, "ember") == 0 || g_strcmp0(norm, "sparks") == 0
	    || g_strcmp0(norm, "hearth") == 0 || g_strcmp0(norm, "fire") == 0) {
		*out = GOWL_BACKDROP_EMBERS;
		return TRUE;
	}
	if (g_strcmp0(norm, "web") == 0 || g_strcmp0(norm, "cobweb") == 0
	    || g_strcmp0(norm, "spiderweb") == 0) {
		*out = GOWL_BACKDROP_DEW;
		return TRUE;
	}
	if (g_strcmp0(norm, "defocus") == 0 || g_strcmp0(norm, "aperture") == 0) {
		*out = GOWL_BACKDROP_BOKEH;
		return TRUE;
	}
	return FALSE;
}

gdouble
gowl_config_get_glass_bevel(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self),
	                     GOWL_CONFIG_DEFAULT_GLASS_BEVEL);
	return self->glass_bevel;
}

gdouble
gowl_config_get_glass_thickness(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self),
	                     GOWL_CONFIG_DEFAULT_GLASS_THICKNESS);
	return self->glass_thickness;
}

gdouble
gowl_config_get_glass_slope(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self),
	                     GOWL_CONFIG_DEFAULT_GLASS_SLOPE);
	return self->glass_slope;
}

const gchar *
gowl_config_get_glass_shape(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self),
	                     GOWL_CONFIG_DEFAULT_GLASS_SHAPE);
	return self->glass_shape;
}

gdouble
gowl_config_get_glass_dispersion(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self),
	                     GOWL_CONFIG_DEFAULT_GLASS_DISPERSION);
	return self->glass_dispersion;
}

gdouble
gowl_config_get_glass_rim(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self),
	                     GOWL_CONFIG_DEFAULT_GLASS_RIM);
	return self->glass_rim;
}

gdouble
gowl_config_get_glass_shade(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self),
	                     GOWL_CONFIG_DEFAULT_GLASS_SHADE);
	return self->glass_shade;
}

gdouble
gowl_config_get_glass_edge_width(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self),
	                     GOWL_CONFIG_DEFAULT_GLASS_EDGE_WIDTH);
	return self->glass_edge_width;
}

gdouble
gowl_config_get_glass_saturation(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self),
	                     GOWL_CONFIG_DEFAULT_GLASS_SATURATION);
	return self->glass_saturation;
}

gdouble
gowl_config_get_glass_clarity(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self),
	                     GOWL_CONFIG_DEFAULT_GLASS_CLARITY);
	return self->glass_clarity;
}

gdouble
gowl_config_get_glass_centre_clarity(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self),
	                     GOWL_CONFIG_DEFAULT_GLASS_CENTRE_CLARITY);
	return self->glass_centre_clarity;
}

gdouble
gowl_config_get_glass_lens(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self),
	                     GOWL_CONFIG_DEFAULT_GLASS_LENS);
	return self->glass_lens;
}

gdouble
gowl_config_get_glass_sheen(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self),
	                     GOWL_CONFIG_DEFAULT_GLASS_SHEEN);
	return self->glass_sheen;
}

gdouble
gowl_config_get_glass_light(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self),
	                     GOWL_CONFIG_DEFAULT_GLASS_LIGHT);
	return self->glass_light;
}

const gchar *
gowl_config_get_glass_tint(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self),
	                     GOWL_CONFIG_DEFAULT_GLASS_TINT);
	return self->glass_tint;
}

gdouble
gowl_config_get_glass_brightness(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self),
	                     GOWL_CONFIG_DEFAULT_GLASS_BRIGHTNESS);
	return self->glass_brightness;
}

gdouble
gowl_config_get_glass_opacity(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self),
	                     GOWL_CONFIG_DEFAULT_GLASS_OPACITY);
	return self->glass_opacity;
}

gint
gowl_config_get_glass_frost(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self),
	                     GOWL_CONFIG_DEFAULT_GLASS_FROST);
	return self->glass_frost;
}

gint
gowl_config_get_glass_frost_passes(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self),
	                     GOWL_CONFIG_DEFAULT_GLASS_FROST_PASSES);
	return self->glass_frost_passes;
}

/* --- Liquid water --- */

/**
 * GowlWaterPreset:
 *
 * One whole tuned set of water.
 *
 * The five of them span what the effect is FOR, and they are a table
 * rather than five sets of defaults because the numbers only mean
 * anything together: a pool's amplitude over a sea's wavelength is not a
 * calmer sea, it is a flat pane with a slow wobble.  Naming the
 * combinations is what lets somebody ask for "a fountain" instead of
 * discovering fourteen numbers.
 *
 * What varies most is not the height.  A pool is its RIPPLES --- four
 * drop sources and almost no swell; a sea has no drops at all and lives
 * entirely in the swell.  Between them the balance shifts.
 */
typedef struct {
	const gchar *name;
	gdouble      amplitude;    /* px */
	gdouble      wavelength;   /* px */
	gdouble      choppiness;   /* 0..1 */
	gdouble      depth;        /* px the refracted ray travels */
	gdouble      drops;        /* how many expanding rings */
	gdouble      drop_amp;     /* how tall, relative to amplitude */
	gdouble      shore;        /* px over which it calms at the edge */
	gdouble      specular;
	gdouble      shine;        /* specular exponent */
	gdouble      fresnel;
	gdouble      caustics;
	gdouble      foam;
	gdouble      absorption;
	gdouble      speed;
} GowlWaterPreset;

static const GowlWaterPreset water_presets[] = {
	/* name        amp   wave  chop  depth drop damp shore  spec shine fres caust foam absorb speed */
	{ "pool",      0.9,   80.0, 0.05,  95.0, 4.0, 3.0, 46.0, 0.35, 88.0, 0.24, 0.30, 0.00, 0.18, 0.40 },
	{ "fountain",  1.5,   95.0, 0.12, 125.0, 6.0, 2.8, 36.0, 0.60, 68.0, 0.36, 0.55, 0.00, 0.26, 1.20 },
	{ "pond",      5.0,  150.0, 0.22, 175.0, 2.0, 1.2, 56.0, 0.55, 44.0, 0.45, 0.50, 0.00, 0.34, 0.60 },
	{ "sea",      16.0,  330.0, 0.58, 250.0, 0.0, 0.0, 28.0, 0.80, 34.0, 0.58, 0.55, 0.16, 0.42, 0.85 },
	{ "storm",    32.0,  430.0, 0.92, 320.0, 1.0, 1.2,  0.0, 0.95, 26.0, 0.66, 0.62, 0.42, 0.48, 1.40 }
};

static const GowlWaterPreset *
water_preset_by_name(const gchar *name)
{
	guint i;

	for (i = 0; i < G_N_ELEMENTS(water_presets); i++) {
		if (g_strcmp0(water_presets[i].name, name) == 0)
			return &water_presets[i];
	}
	/* The pond: the one in the middle, and the shipped default. */
	return &water_presets[2];
}

/* An override wins unless it is the sentinel, in which case the preset
 * decides.  Negative is impossible for every one of these, which is what
 * lets one number carry both the value and "unset". */
static gdouble
water_pick(gdouble override, gdouble from_preset)
{
	return override < 0.0 ? from_preset : override;
}

gboolean
gowl_config_water_preset_valid(const gchar *name)
{
	guint i;

	if (name == NULL)
		return FALSE;
	for (i = 0; i < G_N_ELEMENTS(water_presets); i++) {
		if (g_strcmp0(water_presets[i].name, name) == 0)
			return TRUE;
	}
	return FALSE;
}

const gchar * const *
gowl_config_water_preset_names(void)
{
	static const gchar *names[G_N_ELEMENTS(water_presets) + 1];
	static gsize once = 0;

	if (g_once_init_enter(&once)) {
		guint i;

		for (i = 0; i < G_N_ELEMENTS(water_presets); i++)
			names[i] = water_presets[i].name;
		names[G_N_ELEMENTS(water_presets)] = NULL;
		g_once_init_leave(&once, 1);
	}
	return names;
}

const gchar *
gowl_config_get_water_preset(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self),
	                     GOWL_CONFIG_DEFAULT_WATER_PRESET);
	return self->water_preset;
}

void
gowl_config_set_water_preset(GowlConfig *self, const gchar *name)
{
	g_return_if_fail(GOWL_IS_CONFIG(self));

	if (!gowl_config_water_preset_valid(name))
		return;
	g_free(self->water_preset);
	self->water_preset = g_strdup(name);
}

gdouble
gowl_config_get_water_intensity(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self),
	                     GOWL_CONFIG_DEFAULT_WATER_INTENSITY);
	return self->water_intensity;
}

void
gowl_config_set_water_intensity(GowlConfig *self, gdouble intensity)
{
	g_return_if_fail(GOWL_IS_CONFIG(self));
	self->water_intensity = CLAMP(intensity, 0.0, 3.0);
}

#define GOWL_WATER_GETTER(field)                                           \
gdouble                                                                    \
gowl_config_get_water_##field(GowlConfig *self)                            \
{                                                                          \
	const GowlWaterPreset *p;                                              \
                                                                           \
	g_return_val_if_fail(GOWL_IS_CONFIG(self), 0.0);                       \
	p = water_preset_by_name(self->water_preset);                          \
	return water_pick(self->water_##field, p->field);                      \
}

GOWL_WATER_GETTER(amplitude)
GOWL_WATER_GETTER(wavelength)
GOWL_WATER_GETTER(choppiness)
GOWL_WATER_GETTER(depth)
GOWL_WATER_GETTER(drops)
GOWL_WATER_GETTER(shore)
GOWL_WATER_GETTER(specular)
GOWL_WATER_GETTER(caustics)
GOWL_WATER_GETTER(foam)
GOWL_WATER_GETTER(fresnel)
GOWL_WATER_GETTER(speed)

#undef GOWL_WATER_GETTER

/* Preset-only, with no override key of their own: they are part of what
 * makes a named water what it is, and a config that wants to move them is
 * really asking for a different preset. */
gdouble
gowl_config_get_water_drop_amp(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self), 1.0);
	return water_preset_by_name(self->water_preset)->drop_amp;
}

gdouble
gowl_config_get_water_shine(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self), 48.0);
	return water_preset_by_name(self->water_preset)->shine;
}

gdouble
gowl_config_get_water_absorption(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self), 0.34);
	return water_preset_by_name(self->water_preset)->absorption;
}

/* --- Liquid rain --------------------------------------------------- */

/**
 * GowlRainPreset:
 *
 * One whole tuned set of rain.
 *
 * A table for the same reason the water's is: the numbers only mean
 * anything together.  `density' is a fraction of CELLS, so raising it
 * without raising the cell size makes a finer mist and not heavier rain;
 * `depth' is what inverts the image inside a drop, and it is measured in
 * the drop's OWN RADII so that it means the same thing to the smallest
 * drop and the largest; `life' against `speed' decides
 * whether the pane looks like it is being rained on or like it is drying
 * out.
 *
 * What varies most across the five is not how many drops there are.  It
 * is the balance between drops that SIT and drops that RUN -- a mist is
 * almost entirely resting condensation, and a storm is almost entirely
 * water going down the glass.
 */
typedef struct {
	const gchar *name;
	gdouble      cell;        /* px; the ruler everything else uses */
	gdouble      density;     /* fraction of cells holding a drop */
	gdouble      bulge;       /* how domed */
	gdouble      depth;       /* ray travel, in drop radii */
	gdouble      runs;        /* fraction of columns running */
	gdouble      run_width;   /* px per column */
	gdouble      run_length;  /* px of trail */
	gdouble      beads;       /* how beaded a trail is */
	gdouble      fog;         /* how frosted the dry pane is */
	gdouble      specular;
	gdouble      shine;       /* specular exponent */
	gdouble      rim;
	gdouble      impact;
	gdouble      absorption;
	gdouble      speed;
	gdouble      life;        /* seconds a resting drop lives */
} GowlRainPreset;

static const GowlRainPreset rain_presets[] = {
	/* name       cell dens bulge depth runs  rw    rlen beads fog  spec shine rim  imp  abs  spd  life */
	{ "mist",     48.0, 0.46, 0.75, 5.00, 0.05, 240.0, 240.0, 0.30, 0.94, 0.30, 80.0, 0.22, 0.15, 0.06, 0.50, 16.0 },
	{ "drizzle",  70.0, 0.34, 0.90, 5.50, 0.18, 210.0, 360.0, 0.45, 0.91, 0.38, 70.0, 0.26, 0.35, 0.08, 0.70, 12.0 },
	{ "shower",   95.0, 0.30, 1.00, 6.00, 0.45, 170.0, 520.0, 0.60, 0.88, 0.42, 60.0, 0.30, 0.50, 0.10, 1.00,  8.0 },
	{ "downpour",118.0, 0.34, 1.05, 6.50, 0.70, 140.0, 680.0, 0.72, 0.84, 0.50, 52.0, 0.32, 0.70, 0.12, 1.50,  5.0 },
	{ "storm",   142.0, 0.40, 1.10, 7.00, 0.90, 115.0, 860.0, 0.82, 0.78, 0.58, 46.0, 0.34, 0.85, 0.14, 2.20,  3.5 }
};

static const GowlRainPreset *
rain_preset_by_name(const gchar *name)
{
	guint i;

	for (i = 0; i < G_N_ELEMENTS(rain_presets); i++) {
		if (g_strcmp0(rain_presets[i].name, name) == 0)
			return &rain_presets[i];
	}
	/* The shower: the one in the middle, and the shipped default. */
	return &rain_presets[2];
}

/* An override wins unless it is the sentinel, in which case the preset
 * decides.  Negative is impossible for every one of these. */
static gdouble
rain_pick(gdouble override, gdouble from_preset)
{
	return override < 0.0 ? from_preset : override;
}

gboolean
gowl_config_rain_preset_valid(const gchar *name)
{
	guint i;

	if (name == NULL)
		return FALSE;
	for (i = 0; i < G_N_ELEMENTS(rain_presets); i++) {
		if (g_strcmp0(rain_presets[i].name, name) == 0)
			return TRUE;
	}
	return FALSE;
}

const gchar * const *
gowl_config_rain_preset_names(void)
{
	static const gchar *names[G_N_ELEMENTS(rain_presets) + 1];
	static gsize once = 0;

	if (g_once_init_enter(&once)) {
		guint i;

		for (i = 0; i < G_N_ELEMENTS(rain_presets); i++)
			names[i] = rain_presets[i].name;
		names[G_N_ELEMENTS(rain_presets)] = NULL;
		g_once_init_leave(&once, 1);
	}
	return names;
}

const gchar *
gowl_config_get_rain_preset(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self),
	                     GOWL_CONFIG_DEFAULT_RAIN_PRESET);
	return self->rain_preset;
}

void
gowl_config_set_rain_preset(GowlConfig *self, const gchar *name)
{
	g_return_if_fail(GOWL_IS_CONFIG(self));

	if (!gowl_config_rain_preset_valid(name))
		return;
	g_free(self->rain_preset);
	self->rain_preset = g_strdup(name);
}

gdouble
gowl_config_get_rain_intensity(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self),
	                     GOWL_CONFIG_DEFAULT_RAIN_INTENSITY);
	return self->rain_intensity;
}

void
gowl_config_set_rain_intensity(GowlConfig *self, gdouble intensity)
{
	g_return_if_fail(GOWL_IS_CONFIG(self));
	self->rain_intensity = CLAMP(intensity, 0.0, 3.0);
}

#define GOWL_RAIN_GETTER(field)                                            \
gdouble                                                                    \
gowl_config_get_rain_##field(GowlConfig *self)                             \
{                                                                          \
	const GowlRainPreset *p;                                               \
                                                                           \
	g_return_val_if_fail(GOWL_IS_CONFIG(self), 0.0);                       \
	p = rain_preset_by_name(self->rain_preset);                            \
	return rain_pick(self->rain_##field, p->field);                        \
}

GOWL_RAIN_GETTER(cell)
GOWL_RAIN_GETTER(density)
GOWL_RAIN_GETTER(bulge)
GOWL_RAIN_GETTER(depth)
GOWL_RAIN_GETTER(runs)
GOWL_RAIN_GETTER(run_width)
GOWL_RAIN_GETTER(run_length)
GOWL_RAIN_GETTER(beads)
GOWL_RAIN_GETTER(fog)
GOWL_RAIN_GETTER(specular)
GOWL_RAIN_GETTER(impact)
GOWL_RAIN_GETTER(speed)

#undef GOWL_RAIN_GETTER

/* Preset-only, with no override key of their own: they are part of what
 * makes a named rain what it is, and a config that wants to move them is
 * really asking for a different preset. */
gdouble
gowl_config_get_rain_shine(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self), 60.0);
	return rain_preset_by_name(self->rain_preset)->shine;
}

gdouble
gowl_config_get_rain_rim(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self), 0.35);
	return rain_preset_by_name(self->rain_preset)->rim;
}

gdouble
gowl_config_get_rain_absorption(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self), 0.10);
	return rain_preset_by_name(self->rain_preset)->absorption;
}

gdouble
gowl_config_get_rain_life(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self), 8.0);
	return rain_preset_by_name(self->rain_preset)->life;
}

#define GOWL_BOKEH_DOUBLE(field, lo, hi)                                   \
gdouble                                                                    \
gowl_config_get_bokeh_##field(GowlConfig *self)                            \
{                                                                          \
	g_return_val_if_fail(GOWL_IS_CONFIG(self),                             \
	                     GOWL_CONFIG_DEFAULT_BOKEH_##field##_UC);          \
	return self->bokeh_##field;                                            \
}                                                                          \
void                                                                       \
gowl_config_set_bokeh_##field(GowlConfig *self, gdouble v)                 \
{                                                                          \
	g_return_if_fail(GOWL_IS_CONFIG(self));                                \
	self->bokeh_##field = CLAMP(v, lo, hi);                                \
}

#define GOWL_CONFIG_DEFAULT_BOKEH_radius_UC     GOWL_CONFIG_DEFAULT_BOKEH_RADIUS
#define GOWL_CONFIG_DEFAULT_BOKEH_rotation_UC   GOWL_CONFIG_DEFAULT_BOKEH_ROTATION
#define GOWL_CONFIG_DEFAULT_BOKEH_highlight_UC  GOWL_CONFIG_DEFAULT_BOKEH_HIGHLIGHT
#define GOWL_CONFIG_DEFAULT_BOKEH_threshold_UC  GOWL_CONFIG_DEFAULT_BOKEH_THRESHOLD
#define GOWL_CONFIG_DEFAULT_BOKEH_edge_UC       GOWL_CONFIG_DEFAULT_BOKEH_EDGE
#define GOWL_CONFIG_DEFAULT_BOKEH_brightness_UC GOWL_CONFIG_DEFAULT_BOKEH_BRIGHTNESS

GOWL_BOKEH_DOUBLE(radius, 0.0, 200.0)
GOWL_BOKEH_DOUBLE(rotation, -360.0, 360.0)
GOWL_BOKEH_DOUBLE(highlight, 0.0, 32.0)
GOWL_BOKEH_DOUBLE(threshold, 0.0, 0.99)
GOWL_BOKEH_DOUBLE(edge, 0.0, 2.0)
GOWL_BOKEH_DOUBLE(brightness, 0.0, 2.0)

#undef GOWL_BOKEH_DOUBLE

#define GOWL_BOKEH_INT(field, dflt, lo, hi)                                \
gint                                                                       \
gowl_config_get_bokeh_##field(GowlConfig *self)                            \
{                                                                          \
	g_return_val_if_fail(GOWL_IS_CONFIG(self), dflt);                      \
	return self->bokeh_##field;                                            \
}                                                                          \
void                                                                       \
gowl_config_set_bokeh_##field(GowlConfig *self, gint v)                    \
{                                                                          \
	g_return_if_fail(GOWL_IS_CONFIG(self));                                \
	self->bokeh_##field = CLAMP(v, lo, hi);                                \
}

GOWL_BOKEH_INT(downscale, GOWL_CONFIG_DEFAULT_BOKEH_DOWNSCALE, 1, 4)
GOWL_BOKEH_INT(samples, GOWL_CONFIG_DEFAULT_BOKEH_SAMPLES, 8, 128)
GOWL_BOKEH_INT(blades, GOWL_CONFIG_DEFAULT_BOKEH_BLADES, 0, 12)

#undef GOWL_BOKEH_INT

gboolean
gowl_config_get_rain_lightning(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self),
	                     GOWL_CONFIG_DEFAULT_RAIN_LIGHTNING);
	return self->rain_lightning;
}

void
gowl_config_set_rain_lightning(GowlConfig *self, gboolean on)
{
	g_return_if_fail(GOWL_IS_CONFIG(self));
	self->rain_lightning = on;
}

gdouble
gowl_config_get_rain_lightning_rate(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self),
	                     GOWL_CONFIG_DEFAULT_RAIN_LIGHTNING_RATE);
	return self->rain_lightning_rate;
}

void
gowl_config_set_rain_lightning_rate(GowlConfig *self, gdouble seconds)
{
	g_return_if_fail(GOWL_IS_CONFIG(self));
	self->rain_lightning_rate = CLAMP(seconds, 0.5, 300.0);
}

gdouble
gowl_config_get_rain_lightning_power(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self),
	                     GOWL_CONFIG_DEFAULT_RAIN_LIGHTNING_POWER);
	return self->rain_lightning_power;
}

void
gowl_config_set_rain_lightning_power(GowlConfig *self, gdouble power)
{
	g_return_if_fail(GOWL_IS_CONFIG(self));
	self->rain_lightning_power = CLAMP(power, 0.0, 2.0);
}

gint
gowl_config_get_rain_fps(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self),
	                     GOWL_CONFIG_DEFAULT_RAIN_FPS);
	return self->rain_fps;
}

void
gowl_config_set_rain_fps(GowlConfig *self, gint fps)
{
	g_return_if_fail(GOWL_IS_CONFIG(self));
	self->rain_fps = CLAMP(fps, 0, 144);
}

gint
gowl_config_get_rain_scale(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self),
	                     GOWL_CONFIG_DEFAULT_RAIN_SCALE);
	return self->rain_scale;
}

const gchar *
gowl_config_get_rain_tint(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self),
	                     GOWL_CONFIG_DEFAULT_RAIN_TINT);
	return self->rain_tint;
}

gdouble
gowl_config_get_rain_clarity(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self),
	                     GOWL_CONFIG_DEFAULT_RAIN_CLARITY);
	return self->rain_clarity;
}

gdouble
gowl_config_get_rain_opacity(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self),
	                     GOWL_CONFIG_DEFAULT_RAIN_OPACITY);
	return self->rain_opacity;
}

gdouble
gowl_config_get_rain_brightness(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self),
	                     GOWL_CONFIG_DEFAULT_RAIN_BRIGHTNESS);
	return self->rain_brightness;
}

gdouble
gowl_config_get_rain_light(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self),
	                     GOWL_CONFIG_DEFAULT_RAIN_LIGHT);
	return self->rain_light;
}

gint
gowl_config_get_rain_frost(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self),
	                     GOWL_CONFIG_DEFAULT_RAIN_FROST);
	return self->rain_frost;
}

gint
gowl_config_get_rain_frost_passes(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self),
	                     GOWL_CONFIG_DEFAULT_RAIN_FROST_PASSES);
	return self->rain_frost_passes;
}

/* --- Carbonation ---------------------------------------------------- */

/**
 * GowlFizzPreset:
 *
 * One whole tuned set of carbonation.
 *
 * A table for the same reason the water's and the rain's are: the
 * numbers only mean anything together.  `sites' is a fraction of
 * COLUMNS, so raising it without narrowing `site_width' merely fills in
 * the columns that were already there; `bubble' is a fraction of the
 * column too, and `growth' multiplies it on the way up -- so the two
 * together decide the size at the top and either one alone does not.
 *
 * What varies across the five is not mainly how many bubbles there are.
 * It is how FINE they are and how fast they go.  A champagne is far more
 * carbonated than a cola and its bubbles are a third the size; a flat
 * drink is not a cola with fewer bubbles, it is a cola whose bubbles are
 * all stuck to the glass.
 *
 * The three the user asked for by name are the middle three, and they
 * are a straight scale: sparkling, soda, seltzer.  `flat' and
 * `champagne' extend it at either end.
 *
 * `flat' is not as flat as the name allows, and on purpose.  A drink
 * with genuinely nothing in it is indistinguishable from the effect
 * being switched off, and somebody who has just picked a preset wants to
 * see that they picked one.  So most of what is left is clinging to the
 * glass, which is what a drink that has been sitting out actually looks
 * like, rather than nothing at all.
 */
typedef struct {
	const gchar *name;
	gdouble      cell;        /* px; the clinging layer's lattice */
	gdouble      bubble;      /* release radius, as a share of a column */
	gdouble      growth;      /* how much bigger at the top */
	gdouble      sites;       /* fraction of columns that nucleate */
	gdouble      site_width;  /* px per column; the real ruler */
	gdouble      spacing;     /* how closely a site emits */
	gdouble      stray;       /* loose bubbles between the trains */
	gdouble      cling;       /* fraction of cells holding a stuck one */
	gdouble      wobble;      /* px of sideways wander */
	gdouble      foam;        /* the head */
	gdouble      foam_depth;  /* px */
	gdouble      depth;       /* ray travel, in bubble radii */
	gdouble      mirror;      /* the silvered ring */
	gdouble      fog;         /* how cloudy the drink is */
	gdouble      specular;
	gdouble      shine;       /* specular exponent */
	gdouble      rim;
	gdouble      absorption;
	gdouble      speed;
	gdouble      cling_life;  /* seconds a stuck bubble holds on */
} GowlFizzPreset;

/*
 * THE BUBBLES ARE MUCH BIGGER THAN THE FIRST SET, and the first set was
 * simply wrong.
 *
 * They were sized from what a bubble in a glass measures -- a couple of
 * millimetres, ten or twenty pixels -- which is the right answer to the
 * wrong question.  This is not a photograph of a drink; it is a backdrop
 * seen THROUGH a translucent window, at whatever alpha that window has,
 * behind whatever the application is drawing.  At that size the one
 * thing that identifies a bubble -- the silvered ring around the outer
 * quarter -- was a pixel wide, and the whole effect read as a slightly
 * grubby pane.  A seltzer's release radius is seventeen logical pixels
 * now and half as much again by the time it reaches the top.
 *
 * `depth' moved with them for the same reason: a bubble that minifies
 * only slightly is a disc with a dim ring, and what says LENS is seeing
 * a visibly wider field squeezed into it.
 *
 * `mirror' is 1.0 across the whole table, and that is not laziness.  How
 * bright the ring is is not a property of the drink -- every bubble
 * reflects everything past the critical angle -- so it is the one column
 * with nothing to vary.  `fizz-mirror' is there for anyone who wants it
 * quieter.
 *
 * The ORDERING is what the table is really for and it is unchanged:
 * left to right the bubbles get finer and more numerous, because that is
 * what more carbonation means.  A champagne's bubble at a flat drink's
 * size is neither.
 */
static const GowlFizzPreset fizz_presets[] = {
	/* name         cell   bub    grow  site  sw     spc   stray cling wob   foam  fdep   dep  mir   fog   spec  shine rim   abs   spd   life */
	{ "flat",       200.0, 0.190, 0.35, 0.22, 190.0, 0.26, 0.10, 0.46,  8.0, 0.14,  60.0, 5.2, 1.00, 0.60, 1.20, 80.0, 0.34, 0.16, 0.55, 30.0 },
	{ "sparkling",  175.0, 0.170, 0.48, 0.40, 160.0, 0.48, 0.22, 0.34, 12.0, 0.26,  90.0, 5.0, 1.00, 0.58, 1.35, 74.0, 0.32, 0.15, 0.80, 22.0 },
	{ "soda",       150.0, 0.158, 0.60, 0.62, 138.0, 0.72, 0.32, 0.28, 16.0, 0.38, 110.0, 4.7, 1.00, 0.55, 1.50, 70.0, 0.30, 0.14, 1.00, 14.0 },
	{ "seltzer",    126.0, 0.145, 0.72, 0.85, 118.0, 0.90, 0.44, 0.24, 20.0, 0.50, 130.0, 4.5, 1.00, 0.52, 1.60, 64.0, 0.29, 0.10, 1.30,  9.0 },
	{ "champagne",  104.0, 0.120, 0.85, 1.00,  94.0, 1.00, 0.55, 0.20, 24.0, 0.62, 160.0, 4.2, 1.00, 0.48, 1.75, 58.0, 0.27, 0.07, 1.70,  6.0 }
};

static const GowlFizzPreset *
fizz_preset_by_name(const gchar *name)
{
	guint i;

	for (i = 0; i < G_N_ELEMENTS(fizz_presets); i++) {
		if (g_strcmp0(fizz_presets[i].name, name) == 0)
			return &fizz_presets[i];
	}
	/*
	 * The seltzer, which is the shipped default -- NOT the middle of the
	 * table, which is where the water's and the rain's fallbacks land.
	 * What this answers for an unrecognised name should be what a config
	 * that named nothing gets, and for this one those stopped being the
	 * same entry.
	 */
	return &fizz_presets[3];
}

/* An override wins unless it is the sentinel, in which case the preset
 * decides.  Negative is impossible for every one of these. */
static gdouble
fizz_pick(gdouble override, gdouble from_preset)
{
	return override < 0.0 ? from_preset : override;
}

gboolean
gowl_config_fizz_preset_valid(const gchar *name)
{
	guint i;

	if (name == NULL)
		return FALSE;
	for (i = 0; i < G_N_ELEMENTS(fizz_presets); i++) {
		if (g_strcmp0(fizz_presets[i].name, name) == 0)
			return TRUE;
	}
	return FALSE;
}

const gchar * const *
gowl_config_fizz_preset_names(void)
{
	static const gchar *names[G_N_ELEMENTS(fizz_presets) + 1];
	static gsize once = 0;

	if (g_once_init_enter(&once)) {
		guint i;

		for (i = 0; i < G_N_ELEMENTS(fizz_presets); i++)
			names[i] = fizz_presets[i].name;
		names[G_N_ELEMENTS(fizz_presets)] = NULL;
		g_once_init_leave(&once, 1);
	}
	return names;
}

const gchar *
gowl_config_get_fizz_preset(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self),
	                     GOWL_CONFIG_DEFAULT_FIZZ_PRESET);
	return self->fizz_preset;
}

void
gowl_config_set_fizz_preset(GowlConfig *self, const gchar *name)
{
	g_return_if_fail(GOWL_IS_CONFIG(self));

	if (!gowl_config_fizz_preset_valid(name))
		return;
	g_free(self->fizz_preset);
	self->fizz_preset = g_strdup(name);
}

gdouble
gowl_config_get_fizz_intensity(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self),
	                     GOWL_CONFIG_DEFAULT_FIZZ_INTENSITY);
	return self->fizz_intensity;
}

void
gowl_config_set_fizz_intensity(GowlConfig *self, gdouble intensity)
{
	g_return_if_fail(GOWL_IS_CONFIG(self));
	self->fizz_intensity = CLAMP(intensity, 0.0, 3.0);
}

#define GOWL_FIZZ_GETTER(field)                                            \
gdouble                                                                    \
gowl_config_get_fizz_##field(GowlConfig *self)                             \
{                                                                          \
	const GowlFizzPreset *p;                                               \
                                                                           \
	g_return_val_if_fail(GOWL_IS_CONFIG(self), 0.0);                       \
	p = fizz_preset_by_name(self->fizz_preset);                            \
	return fizz_pick(self->fizz_##field, p->field);                        \
}

GOWL_FIZZ_GETTER(cell)
GOWL_FIZZ_GETTER(bubble)
GOWL_FIZZ_GETTER(growth)
GOWL_FIZZ_GETTER(sites)
GOWL_FIZZ_GETTER(site_width)
GOWL_FIZZ_GETTER(spacing)
GOWL_FIZZ_GETTER(stray)
GOWL_FIZZ_GETTER(cling)
GOWL_FIZZ_GETTER(wobble)
GOWL_FIZZ_GETTER(foam)
GOWL_FIZZ_GETTER(foam_depth)
GOWL_FIZZ_GETTER(depth)
GOWL_FIZZ_GETTER(mirror)
GOWL_FIZZ_GETTER(fog)
GOWL_FIZZ_GETTER(specular)
GOWL_FIZZ_GETTER(speed)

#undef GOWL_FIZZ_GETTER

/* Preset-only, with no override key of their own: they are part of what
 * makes a named drink what it is, and a config that wants to move them
 * is really asking for a different preset. */
gdouble
gowl_config_get_fizz_shine(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self), 70.0);
	return fizz_preset_by_name(self->fizz_preset)->shine;
}

gdouble
gowl_config_get_fizz_rim(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self), 0.28);
	return fizz_preset_by_name(self->fizz_preset)->rim;
}

gdouble
gowl_config_get_fizz_absorption(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self), 0.14);
	return fizz_preset_by_name(self->fizz_preset)->absorption;
}

gdouble
gowl_config_get_fizz_cling_life(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self), 14.0);
	return fizz_preset_by_name(self->fizz_preset)->cling_life;
}

gint
gowl_config_get_fizz_fps(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self),
	                     GOWL_CONFIG_DEFAULT_FIZZ_FPS);
	return self->fizz_fps;
}

void
gowl_config_set_fizz_fps(GowlConfig *self, gint fps)
{
	g_return_if_fail(GOWL_IS_CONFIG(self));
	self->fizz_fps = CLAMP(fps, 0, 144);
}

gint
gowl_config_get_fizz_scale(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self),
	                     GOWL_CONFIG_DEFAULT_FIZZ_SCALE);
	return self->fizz_scale;
}

const gchar *
gowl_config_get_fizz_tint(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self),
	                     GOWL_CONFIG_DEFAULT_FIZZ_TINT);
	return self->fizz_tint;
}

gdouble
gowl_config_get_fizz_clarity(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self),
	                     GOWL_CONFIG_DEFAULT_FIZZ_CLARITY);
	return self->fizz_clarity;
}

gdouble
gowl_config_get_fizz_opacity(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self),
	                     GOWL_CONFIG_DEFAULT_FIZZ_OPACITY);
	return self->fizz_opacity;
}

gdouble
gowl_config_get_fizz_brightness(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self),
	                     GOWL_CONFIG_DEFAULT_FIZZ_BRIGHTNESS);
	return self->fizz_brightness;
}

gdouble
gowl_config_get_fizz_light(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self),
	                     GOWL_CONFIG_DEFAULT_FIZZ_LIGHT);
	return self->fizz_light;
}

gint
gowl_config_get_fizz_frost(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self),
	                     GOWL_CONFIG_DEFAULT_FIZZ_FROST);
	return self->fizz_frost;
}

gint
gowl_config_get_fizz_frost_passes(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self),
	                     GOWL_CONFIG_DEFAULT_FIZZ_FROST_PASSES);
	return self->fizz_frost_passes;
}

/* --- Falling leaves ------------------------------------------------- */

/**
 * GowlLeavesPreset:
 *
 * One whole tuned set of autumn.
 *
 * A table, and here more necessarily than anywhere else in this file,
 * because the numbers are coupled BOTH ways.  `leaf' is capped against
 * `column' and `cell' at render time -- a blade wider than a third of
 * its own column would be sliced off at the column edge -- so asking for
 * bigger leaves without widening the spacing quietly gets smaller ones.
 * And `falling' is a fraction of columns, so narrowing the column to get
 * the leaves bigger also gets fewer of them.
 *
 * What varies across the five is a season and a wind at the same time.
 * `turning' is the first few coming down in still air and they stay on
 * the glass for a long time; `gale' is the same tree in March, where
 * nothing sticks for more than a moment.
 *
 * Even `turning' keeps a couple of leaves on a small window, for the
 * reason `flat' does next door: a preset that is usually EMPTY is
 * indistinguishable from the effect being off, and the tumble already
 * takes half of what is there out of view at any instant.
 */
typedef struct {
	const gchar *name;
	gdouble      leaf;         /* px; blade radius, the ruler */
	gdouble      cell;         /* px per cell of the stuck layer */
	gdouble      stuck;        /* fraction of cells holding one */
	gdouble      column;       /* px per column of the falling layer */
	gdouble      falling;      /* fraction of columns carrying one */
	gdouble      flutter;      /* swing, as a share of a column */
	gdouble      tumble;       /* turns per fall */
	gdouble      wind;         /* steady drift, px per fall */
	gdouble      gust;         /* px a gust throws things */
	gdouble      gustiness;    /* how hard and how often it blows */
	gdouble      curl;         /* how dried out */
	gdouble      veins;
	gdouble      translucency;
	gdouble      gloss;
	gdouble      shadow;
	gdouble      fog;          /* how hazy the pane is */
	gdouble      shine;        /* specular exponent */
	gdouble      speed;
	gdouble      tenure;       /* seconds a leaf holds the glass */
} GowlLeavesPreset;

static const GowlLeavesPreset leaves_presets[] = {
	/* name       leaf  cell  stuck col   fall  flut tumb wind  gust  gy   curl vein tran glos shad fog   shine spd  ten */
	{ "turning",  82.0, 350.0, 0.34, 290.0, 0.55, 0.22, 1.6,  40.0, 110.0, 0.45, 0.18, 0.50, 0.68, 0.12, 0.40, 0.18, 18.0, 0.7, 52.0 },
	{ "autumn",   76.0, 300.0, 0.48, 240.0, 0.85, 0.30, 2.4,  90.0, 220.0, 1.00, 0.35, 0.55, 0.62, 0.16, 0.45, 0.22, 18.0, 1.0, 26.0 },
	{ "peak",     70.0, 250.0, 0.62, 200.0, 1.00, 0.34, 2.9, 130.0, 290.0, 1.20, 0.48, 0.58, 0.58, 0.18, 0.48, 0.24, 17.0, 1.3, 18.0 },
	{ "blustery", 66.0, 265.0, 0.40, 190.0, 0.95, 0.42, 4.2, 260.0, 470.0, 1.70, 0.58, 0.55, 0.55, 0.22, 0.42, 0.26, 16.0, 1.8, 10.0 },
	{ "gale",     60.0, 290.0, 0.22, 170.0, 1.00, 0.45, 6.0, 430.0, 700.0, 2.00, 0.70, 0.50, 0.52, 0.28, 0.36, 0.30, 15.0, 2.6,  5.0 }
};

static const GowlLeavesPreset *
leaves_preset_by_name(const gchar *name)
{
	guint i;

	for (i = 0; i < G_N_ELEMENTS(leaves_presets); i++) {
		if (g_strcmp0(leaves_presets[i].name, name) == 0)
			return &leaves_presets[i];
	}
	/* The autumn: the one in the middle, and the shipped default. */
	return &leaves_presets[1];
}

static gdouble
leaves_pick(gdouble override, gdouble from_preset)
{
	return override < 0.0 ? from_preset : override;
}

gboolean
gowl_config_leaves_preset_valid(const gchar *name)
{
	guint i;

	if (name == NULL)
		return FALSE;
	for (i = 0; i < G_N_ELEMENTS(leaves_presets); i++) {
		if (g_strcmp0(leaves_presets[i].name, name) == 0)
			return TRUE;
	}
	return FALSE;
}

const gchar * const *
gowl_config_leaves_preset_names(void)
{
	static const gchar *names[G_N_ELEMENTS(leaves_presets) + 1];
	static gsize once = 0;

	if (g_once_init_enter(&once)) {
		guint i;

		for (i = 0; i < G_N_ELEMENTS(leaves_presets); i++)
			names[i] = leaves_presets[i].name;
		names[G_N_ELEMENTS(leaves_presets)] = NULL;
		g_once_init_leave(&once, 1);
	}
	return names;
}

const gchar *
gowl_config_get_leaves_preset(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self),
	                     GOWL_CONFIG_DEFAULT_LEAVES_PRESET);
	return self->leaves_preset;
}

void
gowl_config_set_leaves_preset(GowlConfig *self, const gchar *name)
{
	g_return_if_fail(GOWL_IS_CONFIG(self));

	if (!gowl_config_leaves_preset_valid(name))
		return;
	g_free(self->leaves_preset);
	self->leaves_preset = g_strdup(name);
}

gdouble
gowl_config_get_leaves_intensity(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self),
	                     GOWL_CONFIG_DEFAULT_LEAVES_INTENSITY);
	return self->leaves_intensity;
}

void
gowl_config_set_leaves_intensity(GowlConfig *self, gdouble intensity)
{
	g_return_if_fail(GOWL_IS_CONFIG(self));
	self->leaves_intensity = CLAMP(intensity, 0.0, 3.0);
}

#define GOWL_LEAVES_GETTER(field)                                          \
gdouble                                                                    \
gowl_config_get_leaves_##field(GowlConfig *self)                           \
{                                                                          \
	const GowlLeavesPreset *p;                                             \
                                                                           \
	g_return_val_if_fail(GOWL_IS_CONFIG(self), 0.0);                       \
	p = leaves_preset_by_name(self->leaves_preset);                        \
	return leaves_pick(self->leaves_##field, p->field);                    \
}

GOWL_LEAVES_GETTER(leaf)
GOWL_LEAVES_GETTER(cell)
GOWL_LEAVES_GETTER(stuck)
GOWL_LEAVES_GETTER(column)
GOWL_LEAVES_GETTER(falling)
GOWL_LEAVES_GETTER(flutter)
GOWL_LEAVES_GETTER(tumble)
GOWL_LEAVES_GETTER(wind)
GOWL_LEAVES_GETTER(gust)
GOWL_LEAVES_GETTER(gustiness)
GOWL_LEAVES_GETTER(curl)
GOWL_LEAVES_GETTER(veins)
GOWL_LEAVES_GETTER(translucency)
GOWL_LEAVES_GETTER(gloss)
GOWL_LEAVES_GETTER(shadow)
GOWL_LEAVES_GETTER(fog)
GOWL_LEAVES_GETTER(speed)
GOWL_LEAVES_GETTER(tenure)

#undef GOWL_LEAVES_GETTER

gdouble
gowl_config_get_leaves_shine(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self), 18.0);
	return leaves_preset_by_name(self->leaves_preset)->shine;
}

gint
gowl_config_get_leaves_fps(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self),
	                     GOWL_CONFIG_DEFAULT_LEAVES_FPS);
	return self->leaves_fps;
}

void
gowl_config_set_leaves_fps(GowlConfig *self, gint fps)
{
	g_return_if_fail(GOWL_IS_CONFIG(self));
	self->leaves_fps = CLAMP(fps, 0, 144);
}

gint
gowl_config_get_leaves_scale(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self),
	                     GOWL_CONFIG_DEFAULT_LEAVES_SCALE);
	return self->leaves_scale;
}

const gchar *
gowl_config_get_leaves_warm(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self),
	                     GOWL_CONFIG_DEFAULT_LEAVES_WARM);
	return self->leaves_warm;
}

const gchar *
gowl_config_get_leaves_gold(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self),
	                     GOWL_CONFIG_DEFAULT_LEAVES_GOLD);
	return self->leaves_gold;
}

const gchar *
gowl_config_get_leaves_dry(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self),
	                     GOWL_CONFIG_DEFAULT_LEAVES_DRY);
	return self->leaves_dry;
}

gdouble
gowl_config_get_leaves_opacity(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self),
	                     GOWL_CONFIG_DEFAULT_LEAVES_OPACITY);
	return self->leaves_opacity;
}

gdouble
gowl_config_get_leaves_brightness(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self),
	                     GOWL_CONFIG_DEFAULT_LEAVES_BRIGHTNESS);
	return self->leaves_brightness;
}

gdouble
gowl_config_get_leaves_light(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self),
	                     GOWL_CONFIG_DEFAULT_LEAVES_LIGHT);
	return self->leaves_light;
}

gint
gowl_config_get_leaves_frost(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self),
	                     GOWL_CONFIG_DEFAULT_LEAVES_FROST);
	return self->leaves_frost;
}

gint
gowl_config_get_leaves_frost_passes(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self),
	                     GOWL_CONFIG_DEFAULT_LEAVES_FROST_PASSES);
	return self->leaves_frost_passes;
}

/* --- Snow ----------------------------------------------------------- */

/**
 * GowlSnowPreset:
 *
 * One whole tuned set of snow.
 *
 * A table for the usual reason and one more: this one is TWO scales at
 * once.  Left to right it snows harder, and left to right the pane also
 * gets COLDER -- `melt' moves later, `ice' and `ice_rate' go up, `runs'
 * goes down.  A flurry is a warm window that turns everything that lands
 * on it into water within seconds; a blizzard is a cold one that keeps
 * its crystals and grows frost around them.  Picking those apart into
 * separate knobs would be truer to the physics and useless in practice,
 * because nobody wants heavy snow melting instantly.
 *
 * `flake' is the ruler, and it is small: a crystal has to be wide enough
 * for six arms to read, and a window of flakes below that size is dust.
 */
typedef struct {
	const gchar *name;
	gdouble      flake;        /* px; the ruler */
	gdouble      cell;         /* px per cell of the settled layer */
	gdouble      settled;      /* fraction of cells holding one */
	gdouble      column;       /* px per column of the falling layer */
	gdouble      falling;      /* fraction of columns carrying one */
	gdouble      arms;         /* how dendritic */
	gdouble      drift;        /* steady sideways wind, px per fall */
	gdouble      flutter;      /* wander, as a share of a column */
	gdouble      spin;         /* turns per fall */
	gdouble      melt;         /* where in a life the melt begins */
	gdouble      shrink;       /* bead size, as a share of the flake */
	gdouble      depth;        /* ray travel, in bead radii */
	gdouble      runs;         /* melt-water columns running */
	gdouble      run_width;    /* px per run column */
	gdouble      run_length;   /* px of trail */
	gdouble      beads;
	gdouble      ice;          /* frost from the edges */
	gdouble      ice_rate;     /* how fast it creeps in */
	gdouble      ice_scale;    /* px per feather */
	gdouble      sparkle;
	gdouble      fog;
	gdouble      glow;         /* how bright a crystal is */
	gdouble      specular;
	gdouble      shine;
	gdouble      rim;
	gdouble      absorption;
	gdouble      speed;
	gdouble      life;         /* seconds: land, sit, melt, run */
} GowlSnowPreset;

static const GowlSnowPreset snow_presets[] = {
	/* name        flake cell   set   col    fall  arms drift flut spin melt shr  dep  runs rw     rlen  bead ice   irate isc   spk  fog   glow spec shine rim  abs   spd  life */
	{ "flurry",    24.0, 210.0, 0.16, 140.0, 0.34, 0.55, 40.0, 0.20, 0.5, 0.18, 0.32, 5.5, 0.44, 105.0, 340.0, 0.66, 0.10, 0.006, 30.0, 0.40, 0.48, 1.06, 0.42, 62.0, 0.30, 0.10, 0.75, 16.0 },
	{ "light",     27.0, 185.0, 0.22, 115.0, 0.50, 0.65, 80.0, 0.26, 0.6, 0.32, 0.34, 5.5, 0.38, 100.0, 320.0, 0.64, 0.26, 0.012, 27.0, 0.48, 0.54, 1.09, 0.41, 61.0, 0.29, 0.09, 0.88, 22.0 },
	{ "steady",    30.0, 165.0, 0.30,  95.0, 0.70, 0.75, 120.0, 0.30, 0.8, 0.45, 0.35, 5.5, 0.30,  95.0, 300.0, 0.62, 0.45, 0.022, 24.0, 0.55, 0.60, 1.12, 0.40, 60.0, 0.28, 0.08, 1.00, 30.0 },
	{ "heavy",     33.0, 145.0, 0.40,  78.0, 0.88, 0.82, 170.0, 0.34, 1.0, 0.58, 0.36, 5.5, 0.20,  90.0, 260.0, 0.60, 0.62, 0.036, 21.0, 0.62, 0.66, 1.16, 0.38, 58.0, 0.26, 0.07, 1.25, 42.0 },
	{ "blizzard",  36.0, 130.0, 0.52,  62.0, 1.00, 0.90, 260.0, 0.40, 1.4, 0.72, 0.38, 5.5, 0.12,  84.0, 210.0, 0.58, 0.82, 0.055, 18.0, 0.70, 0.74, 1.20, 0.36, 56.0, 0.24, 0.06, 1.60, 60.0 }
};

static const GowlSnowPreset *
snow_preset_by_name(const gchar *name)
{
	guint i;

	for (i = 0; i < G_N_ELEMENTS(snow_presets); i++) {
		if (g_strcmp0(snow_presets[i].name, name) == 0)
			return &snow_presets[i];
	}
	/* The steady fall: the one in the middle, and the shipped default. */
	return &snow_presets[2];
}

static gdouble
snow_pick(gdouble override, gdouble from_preset)
{
	return override < 0.0 ? from_preset : override;
}

gboolean
gowl_config_snow_preset_valid(const gchar *name)
{
	guint i;

	if (name == NULL)
		return FALSE;
	for (i = 0; i < G_N_ELEMENTS(snow_presets); i++) {
		if (g_strcmp0(snow_presets[i].name, name) == 0)
			return TRUE;
	}
	return FALSE;
}

const gchar * const *
gowl_config_snow_preset_names(void)
{
	static const gchar *names[G_N_ELEMENTS(snow_presets) + 1];
	static gsize once = 0;

	if (g_once_init_enter(&once)) {
		guint i;

		for (i = 0; i < G_N_ELEMENTS(snow_presets); i++)
			names[i] = snow_presets[i].name;
		names[G_N_ELEMENTS(snow_presets)] = NULL;
		g_once_init_leave(&once, 1);
	}
	return names;
}

const gchar *
gowl_config_get_snow_preset(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self),
	                     GOWL_CONFIG_DEFAULT_SNOW_PRESET);
	return self->snow_preset;
}

void
gowl_config_set_snow_preset(GowlConfig *self, const gchar *name)
{
	g_return_if_fail(GOWL_IS_CONFIG(self));

	if (!gowl_config_snow_preset_valid(name))
		return;
	g_free(self->snow_preset);
	self->snow_preset = g_strdup(name);
}

gdouble
gowl_config_get_snow_intensity(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self),
	                     GOWL_CONFIG_DEFAULT_SNOW_INTENSITY);
	return self->snow_intensity;
}

void
gowl_config_set_snow_intensity(GowlConfig *self, gdouble intensity)
{
	g_return_if_fail(GOWL_IS_CONFIG(self));
	self->snow_intensity = CLAMP(intensity, 0.0, 3.0);
}

#define GOWL_SNOW_GETTER(field)                                            \
gdouble                                                                    \
gowl_config_get_snow_##field(GowlConfig *self)                             \
{                                                                          \
	const GowlSnowPreset *p;                                               \
                                                                           \
	g_return_val_if_fail(GOWL_IS_CONFIG(self), 0.0);                       \
	p = snow_preset_by_name(self->snow_preset);                            \
	return snow_pick(self->snow_##field, p->field);                        \
}

GOWL_SNOW_GETTER(flake)
GOWL_SNOW_GETTER(cell)
GOWL_SNOW_GETTER(settled)
GOWL_SNOW_GETTER(column)
GOWL_SNOW_GETTER(falling)
GOWL_SNOW_GETTER(arms)
GOWL_SNOW_GETTER(drift)
GOWL_SNOW_GETTER(flutter)
GOWL_SNOW_GETTER(spin)
GOWL_SNOW_GETTER(melt)
GOWL_SNOW_GETTER(shrink)
GOWL_SNOW_GETTER(depth)
GOWL_SNOW_GETTER(runs)
GOWL_SNOW_GETTER(run_width)
GOWL_SNOW_GETTER(run_length)
GOWL_SNOW_GETTER(beads)
GOWL_SNOW_GETTER(ice)
GOWL_SNOW_GETTER(ice_rate)
GOWL_SNOW_GETTER(ice_scale)
GOWL_SNOW_GETTER(sparkle)
GOWL_SNOW_GETTER(fog)
GOWL_SNOW_GETTER(glow)
GOWL_SNOW_GETTER(specular)
GOWL_SNOW_GETTER(speed)

#undef GOWL_SNOW_GETTER

gdouble
gowl_config_get_snow_shine(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self), 60.0);
	return snow_preset_by_name(self->snow_preset)->shine;
}

gdouble
gowl_config_get_snow_rim(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self), 0.28);
	return snow_preset_by_name(self->snow_preset)->rim;
}

gdouble
gowl_config_get_snow_absorption(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self), 0.08);
	return snow_preset_by_name(self->snow_preset)->absorption;
}

gdouble
gowl_config_get_snow_life(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self), 30.0);
	return snow_preset_by_name(self->snow_preset)->life;
}

gint
gowl_config_get_snow_fps(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self),
	                     GOWL_CONFIG_DEFAULT_SNOW_FPS);
	return self->snow_fps;
}

void
gowl_config_set_snow_fps(GowlConfig *self, gint fps)
{
	g_return_if_fail(GOWL_IS_CONFIG(self));
	self->snow_fps = CLAMP(fps, 0, 144);
}

gint
gowl_config_get_snow_scale(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self),
	                     GOWL_CONFIG_DEFAULT_SNOW_SCALE);
	return self->snow_scale;
}

const gchar *
gowl_config_get_snow_tint(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self),
	                     GOWL_CONFIG_DEFAULT_SNOW_TINT);
	return self->snow_tint;
}

gdouble
gowl_config_get_snow_clarity(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self),
	                     GOWL_CONFIG_DEFAULT_SNOW_CLARITY);
	return self->snow_clarity;
}

gdouble
gowl_config_get_snow_opacity(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self),
	                     GOWL_CONFIG_DEFAULT_SNOW_OPACITY);
	return self->snow_opacity;
}

gdouble
gowl_config_get_snow_brightness(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self),
	                     GOWL_CONFIG_DEFAULT_SNOW_BRIGHTNESS);
	return self->snow_brightness;
}

gdouble
gowl_config_get_snow_light(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self),
	                     GOWL_CONFIG_DEFAULT_SNOW_LIGHT);
	return self->snow_light;
}

gint
gowl_config_get_snow_frost(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self),
	                     GOWL_CONFIG_DEFAULT_SNOW_FROST);
	return self->snow_frost;
}

gint
gowl_config_get_snow_frost_passes(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self),
	                     GOWL_CONFIG_DEFAULT_SNOW_FROST_PASSES);
	return self->snow_frost_passes;
}

/* --- The soap film, the embers, the submerged view and the dew ----- */

typedef struct {
	const gchar *name;
	gdouble      thickness;
	gdouble      thin;
	gdouble      drain;
	gdouble      turbulence;
	gdouble      swirl;
	gdouble      index;
	gdouble      gain;
	gdouble      sheen;
	gdouble      wedge;
	gdouble      pop;
	gdouble      meniscus;
	gdouble      fog;
	gdouble      speed;
	gdouble      life;
} GowlSoapPreset;

static const GowlSoapPreset soap_presets[] = {
	/* thickness thin drain turbulence swirl index gain sheen wedge pop meniscus fog speed life */
	{ "fresh", 1400.0, 0.22, 0.85, 0.42, 2.6, 1.35, 4.5, 0.6, 260.0, 0.08, 0.7, 0.32, 0.8, 34.0 },
	{ "drifting", 900.0, 0.1, 1.3, 0.55, 3.2, 1.35, 5.0, 0.55, 220.0, 0.1, 0.55, 0.3, 1.0, 18.0 },
	{ "thin", 520.0, 0.05, 2.1, 0.75, 4.1, 1.35, 5.6, 0.5, 180.0, 0.14, 0.4, 0.28, 1.3, 11.0 },
	{ "oil", 1150.0, 0.55, 0.1, 0.35, 2.2, 1.47, 4.0, 0.35, 300.0, 0.0, 0.2, 0.36, 0.45, 60.0 },
	{ "bubble", 700.0, 0.06, 2.6, 0.95, 4.8, 1.33, 6.0, 0.65, 200.0, 0.18, 0.45, 0.26, 1.7, 7.0 }
};

static const GowlSoapPreset *
soap_preset_by_name(const gchar *name)
{
	guint i;

	for (i = 0; i < G_N_ELEMENTS(soap_presets); i++) {
		if (g_strcmp0(soap_presets[i].name, name) == 0)
			return &soap_presets[i];
	}
	/* The shipped default, so an unknown name behaves exactly
	 * like naming none. */
	return &soap_presets[1];
}

gboolean
gowl_config_soap_preset_valid(const gchar *name)
{
	guint i;

	if (name == NULL)
		return FALSE;
	for (i = 0; i < G_N_ELEMENTS(soap_presets); i++) {
		if (g_strcmp0(soap_presets[i].name, name) == 0)
			return TRUE;
	}
	return FALSE;
}

const gchar * const *
gowl_config_soap_preset_names(void)
{
	static const gchar *names[G_N_ELEMENTS(soap_presets) + 1];
	static gsize once = 0;

	if (g_once_init_enter(&once)) {
		guint i;

		for (i = 0; i < G_N_ELEMENTS(soap_presets); i++)
			names[i] = soap_presets[i].name;
		names[G_N_ELEMENTS(soap_presets)] = NULL;
		g_once_init_leave(&once, 1);
	}
	return names;
}

const gchar *
gowl_config_get_soap_preset(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self),
	                     GOWL_CONFIG_DEFAULT_SOAP_PRESET);
	return self->soap_preset;
}

void
gowl_config_set_soap_preset(GowlConfig *self, const gchar *name)
{
	g_return_if_fail(GOWL_IS_CONFIG(self));

	if (!gowl_config_soap_preset_valid(name))
		return;
	g_free(self->soap_preset);
	self->soap_preset = g_strdup(name);
}

/* An override wins unless it is the sentinel, in which case the
 * preset decides. */
#define GOWL_SOAP_GETTER(field)                                        \
gdouble                                                            \
gowl_config_get_soap_##field(GowlConfig *self)                   \
{                                                                  \
	const GowlSoapPreset *p;                                       \
                                                                   \
	g_return_val_if_fail(GOWL_IS_CONFIG(self), 0.0);               \
	p = soap_preset_by_name(self->soap_preset);                  \
	return self->soap_##field < 0.0 ? p->field                    \
	                                : self->soap_##field;         \
}

GOWL_SOAP_GETTER(thickness)
GOWL_SOAP_GETTER(thin)
GOWL_SOAP_GETTER(drain)
GOWL_SOAP_GETTER(turbulence)
GOWL_SOAP_GETTER(swirl)
GOWL_SOAP_GETTER(index)
GOWL_SOAP_GETTER(gain)
GOWL_SOAP_GETTER(sheen)
GOWL_SOAP_GETTER(wedge)
GOWL_SOAP_GETTER(pop)
GOWL_SOAP_GETTER(meniscus)
GOWL_SOAP_GETTER(fog)
GOWL_SOAP_GETTER(speed)
GOWL_SOAP_GETTER(life)

#undef GOWL_SOAP_GETTER

const gchar *
gowl_config_get_soap_tint(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self),
	                     GOWL_CONFIG_DEFAULT_SOAP_TINT);
	return self->soap_tint;
}

gdouble
gowl_config_get_soap_intensity(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self), GOWL_CONFIG_DEFAULT_SOAP_INTENSITY);
	return self->soap_intensity;
}

gdouble
gowl_config_get_soap_clarity(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self), GOWL_CONFIG_DEFAULT_SOAP_CLARITY);
	return self->soap_clarity;
}

gdouble
gowl_config_get_soap_opacity(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self), GOWL_CONFIG_DEFAULT_SOAP_OPACITY);
	return self->soap_opacity;
}

gdouble
gowl_config_get_soap_brightness(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self), GOWL_CONFIG_DEFAULT_SOAP_BRIGHTNESS);
	return self->soap_brightness;
}

gdouble
gowl_config_get_soap_light(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self), GOWL_CONFIG_DEFAULT_SOAP_LIGHT);
	return self->soap_light;
}

gint
gowl_config_get_soap_fps(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self), GOWL_CONFIG_DEFAULT_SOAP_FPS);
	return self->soap_fps;
}

gint
gowl_config_get_soap_scale(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self), GOWL_CONFIG_DEFAULT_SOAP_SCALE);
	return self->soap_scale;
}

gint
gowl_config_get_soap_frost(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self), GOWL_CONFIG_DEFAULT_SOAP_FROST);
	return self->soap_frost;
}

gint
gowl_config_get_soap_frost_passes(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self), GOWL_CONFIG_DEFAULT_SOAP_FROST_PASSES);
	return self->soap_frost_passes;
}


typedef struct {
	const gchar *name;
	gdouble      column;
	gdouble      density;
	gdouble      ember;
	gdouble      spacing;
	gdouble      sway;
	gdouble      drag;
	gdouble      temperature;
	gdouble      cool;
	gdouble      flicker;
	gdouble      ash;
	gdouble      glow;
	gdouble      hearth;
	gdouble      haze;
	gdouble      haze_scale;
	gdouble      fog;
	gdouble      speed;
} GowlEmbersPreset;

static const GowlEmbersPreset embers_presets[] = {
	/* column density ember spacing sway drag temperature cool flicker ash glow hearth haze haze_scale fog speed */
	{ "dying", 118.0, 0.48, 4.4, 0.38, 14.0, 2.6, 1500.0, 0.95, 0.65, 0.34, 1.05, 0.36, 5.0, 3.0, 0.34, 0.7 },
	{ "embers", 84.0, 0.85, 5.5, 0.80, 18.0, 2.2, 2300.0, 0.65, 0.55, 0.16, 1.50, 0.62, 9.0, 3.4, 0.3, 1.0 },
	{ "campfire", 70.0, 0.92, 6.2, 0.95, 22.0, 2.0, 2500.0, 0.58, 0.5, 0.12, 1.75, 0.76, 13.0, 3.8, 0.28, 1.3 },
	{ "forge", 58.0, 1.0, 6.6, 1.00, 16.0, 1.8, 2900.0, 0.46, 0.4, 0.05, 2.00, 0.92, 17.0, 4.2, 0.24, 1.55 },
	{ "wildfire", 52.0, 1.0, 7.2, 1.00, 30.0, 1.6, 2700.0, 0.52, 0.7, 0.18, 2.30, 1.05, 22.0, 4.6, 0.22, 2.0 }
};

static const GowlEmbersPreset *
embers_preset_by_name(const gchar *name)
{
	guint i;

	for (i = 0; i < G_N_ELEMENTS(embers_presets); i++) {
		if (g_strcmp0(embers_presets[i].name, name) == 0)
			return &embers_presets[i];
	}
	/* The shipped default, so an unknown name behaves exactly
	 * like naming none. */
	return &embers_presets[1];
}

gboolean
gowl_config_embers_preset_valid(const gchar *name)
{
	guint i;

	if (name == NULL)
		return FALSE;
	for (i = 0; i < G_N_ELEMENTS(embers_presets); i++) {
		if (g_strcmp0(embers_presets[i].name, name) == 0)
			return TRUE;
	}
	return FALSE;
}

const gchar * const *
gowl_config_embers_preset_names(void)
{
	static const gchar *names[G_N_ELEMENTS(embers_presets) + 1];
	static gsize once = 0;

	if (g_once_init_enter(&once)) {
		guint i;

		for (i = 0; i < G_N_ELEMENTS(embers_presets); i++)
			names[i] = embers_presets[i].name;
		names[G_N_ELEMENTS(embers_presets)] = NULL;
		g_once_init_leave(&once, 1);
	}
	return names;
}

const gchar *
gowl_config_get_embers_preset(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self),
	                     GOWL_CONFIG_DEFAULT_EMBERS_PRESET);
	return self->embers_preset;
}

void
gowl_config_set_embers_preset(GowlConfig *self, const gchar *name)
{
	g_return_if_fail(GOWL_IS_CONFIG(self));

	if (!gowl_config_embers_preset_valid(name))
		return;
	g_free(self->embers_preset);
	self->embers_preset = g_strdup(name);
}

/* An override wins unless it is the sentinel, in which case the
 * preset decides. */
#define GOWL_EMBERS_GETTER(field)                                        \
gdouble                                                            \
gowl_config_get_embers_##field(GowlConfig *self)                   \
{                                                                  \
	const GowlEmbersPreset *p;                                       \
                                                                   \
	g_return_val_if_fail(GOWL_IS_CONFIG(self), 0.0);               \
	p = embers_preset_by_name(self->embers_preset);                  \
	return self->embers_##field < 0.0 ? p->field                    \
	                                : self->embers_##field;         \
}

GOWL_EMBERS_GETTER(column)
GOWL_EMBERS_GETTER(density)
GOWL_EMBERS_GETTER(ember)
GOWL_EMBERS_GETTER(spacing)
GOWL_EMBERS_GETTER(sway)
GOWL_EMBERS_GETTER(drag)
GOWL_EMBERS_GETTER(temperature)
GOWL_EMBERS_GETTER(cool)
GOWL_EMBERS_GETTER(flicker)
GOWL_EMBERS_GETTER(ash)
GOWL_EMBERS_GETTER(glow)
GOWL_EMBERS_GETTER(hearth)
GOWL_EMBERS_GETTER(haze)
GOWL_EMBERS_GETTER(haze_scale)
GOWL_EMBERS_GETTER(fog)
GOWL_EMBERS_GETTER(speed)

#undef GOWL_EMBERS_GETTER

const gchar *
gowl_config_get_embers_tint(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self),
	                     GOWL_CONFIG_DEFAULT_EMBERS_TINT);
	return self->embers_tint;
}

gdouble
gowl_config_get_embers_intensity(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self), GOWL_CONFIG_DEFAULT_EMBERS_INTENSITY);
	return self->embers_intensity;
}

gdouble
gowl_config_get_embers_clarity(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self), GOWL_CONFIG_DEFAULT_EMBERS_CLARITY);
	return self->embers_clarity;
}

gdouble
gowl_config_get_embers_opacity(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self), GOWL_CONFIG_DEFAULT_EMBERS_OPACITY);
	return self->embers_opacity;
}

gdouble
gowl_config_get_embers_brightness(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self), GOWL_CONFIG_DEFAULT_EMBERS_BRIGHTNESS);
	return self->embers_brightness;
}

gint
gowl_config_get_embers_fps(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self), GOWL_CONFIG_DEFAULT_EMBERS_FPS);
	return self->embers_fps;
}

gint
gowl_config_get_embers_scale(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self), GOWL_CONFIG_DEFAULT_EMBERS_SCALE);
	return self->embers_scale;
}

gint
gowl_config_get_embers_frost(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self), GOWL_CONFIG_DEFAULT_EMBERS_FROST);
	return self->embers_frost;
}

gint
gowl_config_get_embers_frost_passes(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self), GOWL_CONFIG_DEFAULT_EMBERS_FROST_PASSES);
	return self->embers_frost_passes;
}


typedef struct {
	const gchar *name;
	gdouble      depth;
	gdouble      extinction_r;
	gdouble      extinction_g;
	gdouble      extinction_b;
	gdouble      murk;
	gdouble      caustics;
	gdouble      caustic_scale;
	gdouble      shafts;
	gdouble      shaft_lean;
	gdouble      motes;
	gdouble      mote_size;
	gdouble      surface;
	gdouble      sway;
	gdouble      fog;
	gdouble      speed;
	gdouble      drift;
} GowlSubmergedPreset;

static const GowlSubmergedPreset submerged_presets[] = {
	/* depth extinction_r extinction_g extinction_b murk caustics caustic_scale shafts shaft_lean motes mote_size surface sway fog speed drift */
	{ "pool", 1.4, 0.3, 0.06, 0.04, 0.04, 0.85, 5.6, 0.3, 0.35, 0.18, 1.8, 0.65, 5.0, 0.16, 1.2, 30.0 },
	{ "reef", 3.4, 0.42, 0.075, 0.025, 0.12, 0.55, 4.2, 0.22, 0.35, 0.38, 2.2, 0.45, 6.0, 0.22, 1.0, 22.0 },
	{ "lake", 4.5, 0.5, 0.22, 0.28, 0.35, 0.28, 3.4, 0.14, 0.3, 0.55, 2.6, 0.3, 5.0, 0.3, 0.75, 26.0 },
	{ "deep", 9.0, 0.46, 0.1, 0.02, 0.18, 0.12, 2.6, 0.3, 0.2, 0.62, 2.0, 0.1, 4.0, 0.26, 0.6, 34.0 },
	{ "murk", 6.0, 0.55, 0.3, 0.34, 0.85, 0.1, 3.0, 0.06, 0.25, 0.75, 3.0, 0.14, 4.5, 0.42, 0.55, 20.0 }
};

static const GowlSubmergedPreset *
submerged_preset_by_name(const gchar *name)
{
	guint i;

	for (i = 0; i < G_N_ELEMENTS(submerged_presets); i++) {
		if (g_strcmp0(submerged_presets[i].name, name) == 0)
			return &submerged_presets[i];
	}
	/* The shipped default, so an unknown name behaves exactly
	 * like naming none. */
	return &submerged_presets[1];
}

gboolean
gowl_config_submerged_preset_valid(const gchar *name)
{
	guint i;

	if (name == NULL)
		return FALSE;
	for (i = 0; i < G_N_ELEMENTS(submerged_presets); i++) {
		if (g_strcmp0(submerged_presets[i].name, name) == 0)
			return TRUE;
	}
	return FALSE;
}

const gchar * const *
gowl_config_submerged_preset_names(void)
{
	static const gchar *names[G_N_ELEMENTS(submerged_presets) + 1];
	static gsize once = 0;

	if (g_once_init_enter(&once)) {
		guint i;

		for (i = 0; i < G_N_ELEMENTS(submerged_presets); i++)
			names[i] = submerged_presets[i].name;
		names[G_N_ELEMENTS(submerged_presets)] = NULL;
		g_once_init_leave(&once, 1);
	}
	return names;
}

const gchar *
gowl_config_get_submerged_preset(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self),
	                     GOWL_CONFIG_DEFAULT_SUBMERGED_PRESET);
	return self->submerged_preset;
}

void
gowl_config_set_submerged_preset(GowlConfig *self, const gchar *name)
{
	g_return_if_fail(GOWL_IS_CONFIG(self));

	if (!gowl_config_submerged_preset_valid(name))
		return;
	g_free(self->submerged_preset);
	self->submerged_preset = g_strdup(name);
}

/* An override wins unless it is the sentinel, in which case the
 * preset decides. */
#define GOWL_SUBMERGED_GETTER(field)                                        \
gdouble                                                            \
gowl_config_get_submerged_##field(GowlConfig *self)                   \
{                                                                  \
	const GowlSubmergedPreset *p;                                       \
                                                                   \
	g_return_val_if_fail(GOWL_IS_CONFIG(self), 0.0);               \
	p = submerged_preset_by_name(self->submerged_preset);                  \
	return self->submerged_##field < 0.0 ? p->field                    \
	                                : self->submerged_##field;         \
}

GOWL_SUBMERGED_GETTER(depth)
GOWL_SUBMERGED_GETTER(extinction_r)
GOWL_SUBMERGED_GETTER(extinction_g)
GOWL_SUBMERGED_GETTER(extinction_b)
GOWL_SUBMERGED_GETTER(murk)
GOWL_SUBMERGED_GETTER(caustics)
GOWL_SUBMERGED_GETTER(caustic_scale)
GOWL_SUBMERGED_GETTER(shafts)
GOWL_SUBMERGED_GETTER(shaft_lean)
GOWL_SUBMERGED_GETTER(motes)
GOWL_SUBMERGED_GETTER(mote_size)
GOWL_SUBMERGED_GETTER(surface)
GOWL_SUBMERGED_GETTER(sway)
GOWL_SUBMERGED_GETTER(fog)
GOWL_SUBMERGED_GETTER(speed)
GOWL_SUBMERGED_GETTER(drift)

#undef GOWL_SUBMERGED_GETTER

const gchar *
gowl_config_get_submerged_water(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self),
	                     GOWL_CONFIG_DEFAULT_SUBMERGED_WATER);
	return self->submerged_water;
}

gdouble
gowl_config_get_submerged_intensity(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self), GOWL_CONFIG_DEFAULT_SUBMERGED_INTENSITY);
	return self->submerged_intensity;
}

gdouble
gowl_config_get_submerged_clarity(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self), GOWL_CONFIG_DEFAULT_SUBMERGED_CLARITY);
	return self->submerged_clarity;
}

gdouble
gowl_config_get_submerged_opacity(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self), GOWL_CONFIG_DEFAULT_SUBMERGED_OPACITY);
	return self->submerged_opacity;
}

gdouble
gowl_config_get_submerged_brightness(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self), GOWL_CONFIG_DEFAULT_SUBMERGED_BRIGHTNESS);
	return self->submerged_brightness;
}

gint
gowl_config_get_submerged_fps(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self), GOWL_CONFIG_DEFAULT_SUBMERGED_FPS);
	return self->submerged_fps;
}

gint
gowl_config_get_submerged_scale(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self), GOWL_CONFIG_DEFAULT_SUBMERGED_SCALE);
	return self->submerged_scale;
}

gint
gowl_config_get_submerged_frost(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self), GOWL_CONFIG_DEFAULT_SUBMERGED_FROST);
	return self->submerged_frost;
}

gint
gowl_config_get_submerged_frost_passes(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self), GOWL_CONFIG_DEFAULT_SUBMERGED_FROST_PASSES);
	return self->submerged_frost_passes;
}


typedef struct {
	const gchar *name;
	gdouble      radials;
	gdouble      pitch;
	gdouble      thread;
	gdouble      drop;
	gdouble      spacing;
	gdouble      sag;
	gdouble      depth;
	gdouble      bulge;
	gdouble      silk;
	gdouble      glint;
	gdouble      shine;
	gdouble      rim;
	gdouble      sway;
	gdouble      fog;
	gdouble      speed;
} GowlDewPreset;

static const GowlDewPreset dew_presets[] = {
	/* radials pitch thread drop spacing sag depth bulge silk glint shine rim sway fog speed */
	{ "gossamer", 22.0, 54.0, 1.0, 5.6, 30.0, 4.0, 5.0, 1.0, 0.26, 1.10, 66.0, 0.24, 2.6, 0.3, 1.0 },
	{ "dawn", 15.0, 74.0, 1.3, 9.0, 46.0, 7.0, 5.6, 1.0, 0.30, 1.25, 58.0, 0.26, 3.2, 0.34, 1.0 },
	{ "heavy", 12.0, 92.0, 1.7, 13.5, 60.0, 12.0, 6.2, 1.05, 0.34, 1.45, 50.0, 0.3, 4.2, 0.38, 0.8 },
	{ "tattered", 9.0, 118.0, 1.4, 10.0, 72.0, 9.0, 5.4, 0.95, 0.22, 1.15, 60.0, 0.26, 5.0, 0.28, 1.2 },
	{ "frostweb", 19.0, 62.0, 2.0, 4.0, 26.0, 2.0, 4.2, 0.9, 0.62, 0.90, 74.0, 0.18, 1.8, 0.44, 0.6 }
};

static const GowlDewPreset *
dew_preset_by_name(const gchar *name)
{
	guint i;

	for (i = 0; i < G_N_ELEMENTS(dew_presets); i++) {
		if (g_strcmp0(dew_presets[i].name, name) == 0)
			return &dew_presets[i];
	}
	/* The shipped default, so an unknown name behaves exactly
	 * like naming none. */
	return &dew_presets[1];
}

gboolean
gowl_config_dew_preset_valid(const gchar *name)
{
	guint i;

	if (name == NULL)
		return FALSE;
	for (i = 0; i < G_N_ELEMENTS(dew_presets); i++) {
		if (g_strcmp0(dew_presets[i].name, name) == 0)
			return TRUE;
	}
	return FALSE;
}

const gchar * const *
gowl_config_dew_preset_names(void)
{
	static const gchar *names[G_N_ELEMENTS(dew_presets) + 1];
	static gsize once = 0;

	if (g_once_init_enter(&once)) {
		guint i;

		for (i = 0; i < G_N_ELEMENTS(dew_presets); i++)
			names[i] = dew_presets[i].name;
		names[G_N_ELEMENTS(dew_presets)] = NULL;
		g_once_init_leave(&once, 1);
	}
	return names;
}

const gchar *
gowl_config_get_dew_preset(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self),
	                     GOWL_CONFIG_DEFAULT_DEW_PRESET);
	return self->dew_preset;
}

void
gowl_config_set_dew_preset(GowlConfig *self, const gchar *name)
{
	g_return_if_fail(GOWL_IS_CONFIG(self));

	if (!gowl_config_dew_preset_valid(name))
		return;
	g_free(self->dew_preset);
	self->dew_preset = g_strdup(name);
}

/* An override wins unless it is the sentinel, in which case the
 * preset decides. */
#define GOWL_DEW_GETTER(field)                                        \
gdouble                                                            \
gowl_config_get_dew_##field(GowlConfig *self)                   \
{                                                                  \
	const GowlDewPreset *p;                                       \
                                                                   \
	g_return_val_if_fail(GOWL_IS_CONFIG(self), 0.0);               \
	p = dew_preset_by_name(self->dew_preset);                  \
	return self->dew_##field < 0.0 ? p->field                    \
	                                : self->dew_##field;         \
}

GOWL_DEW_GETTER(radials)
GOWL_DEW_GETTER(pitch)
GOWL_DEW_GETTER(thread)
GOWL_DEW_GETTER(drop)
GOWL_DEW_GETTER(spacing)
GOWL_DEW_GETTER(sag)
GOWL_DEW_GETTER(depth)
GOWL_DEW_GETTER(bulge)
GOWL_DEW_GETTER(silk)
GOWL_DEW_GETTER(glint)
GOWL_DEW_GETTER(shine)
GOWL_DEW_GETTER(rim)
GOWL_DEW_GETTER(sway)
GOWL_DEW_GETTER(fog)
GOWL_DEW_GETTER(speed)

#undef GOWL_DEW_GETTER

const gchar *
gowl_config_get_dew_tint(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self),
	                     GOWL_CONFIG_DEFAULT_DEW_TINT);
	return self->dew_tint;
}

gdouble
gowl_config_get_dew_intensity(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self), GOWL_CONFIG_DEFAULT_DEW_INTENSITY);
	return self->dew_intensity;
}

gdouble
gowl_config_get_dew_clarity(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self), GOWL_CONFIG_DEFAULT_DEW_CLARITY);
	return self->dew_clarity;
}

gdouble
gowl_config_get_dew_opacity(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self), GOWL_CONFIG_DEFAULT_DEW_OPACITY);
	return self->dew_opacity;
}

gdouble
gowl_config_get_dew_brightness(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self), GOWL_CONFIG_DEFAULT_DEW_BRIGHTNESS);
	return self->dew_brightness;
}

gdouble
gowl_config_get_dew_light(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self), GOWL_CONFIG_DEFAULT_DEW_LIGHT);
	return self->dew_light;
}

gint
gowl_config_get_dew_fps(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self), GOWL_CONFIG_DEFAULT_DEW_FPS);
	return self->dew_fps;
}

gint
gowl_config_get_dew_scale(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self), GOWL_CONFIG_DEFAULT_DEW_SCALE);
	return self->dew_scale;
}

gint
gowl_config_get_dew_frost(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self), GOWL_CONFIG_DEFAULT_DEW_FROST);
	return self->dew_frost;
}

gint
gowl_config_get_dew_frost_passes(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self), GOWL_CONFIG_DEFAULT_DEW_FROST_PASSES);
	return self->dew_frost_passes;
}


void
gowl_config_set_soap_intensity(GowlConfig *self, gdouble intensity)
{
	g_return_if_fail(GOWL_IS_CONFIG(self));
	self->soap_intensity = CLAMP(intensity, 0.0, 3.0);
}

void
gowl_config_set_soap_fps(GowlConfig *self, gint fps)
{
	g_return_if_fail(GOWL_IS_CONFIG(self));
	self->soap_fps = CLAMP(fps, 0, 144);
}

void
gowl_config_set_soap_scale(GowlConfig *self, gint scale)
{
	g_return_if_fail(GOWL_IS_CONFIG(self));
	self->soap_scale = CLAMP(scale, 1, 4);
}

void
gowl_config_set_embers_intensity(GowlConfig *self, gdouble intensity)
{
	g_return_if_fail(GOWL_IS_CONFIG(self));
	self->embers_intensity = CLAMP(intensity, 0.0, 3.0);
}

void
gowl_config_set_embers_fps(GowlConfig *self, gint fps)
{
	g_return_if_fail(GOWL_IS_CONFIG(self));
	self->embers_fps = CLAMP(fps, 0, 144);
}

void
gowl_config_set_embers_scale(GowlConfig *self, gint scale)
{
	g_return_if_fail(GOWL_IS_CONFIG(self));
	self->embers_scale = CLAMP(scale, 1, 4);
}

void
gowl_config_set_submerged_intensity(GowlConfig *self, gdouble intensity)
{
	g_return_if_fail(GOWL_IS_CONFIG(self));
	self->submerged_intensity = CLAMP(intensity, 0.0, 3.0);
}

void
gowl_config_set_submerged_fps(GowlConfig *self, gint fps)
{
	g_return_if_fail(GOWL_IS_CONFIG(self));
	self->submerged_fps = CLAMP(fps, 0, 144);
}

void
gowl_config_set_submerged_scale(GowlConfig *self, gint scale)
{
	g_return_if_fail(GOWL_IS_CONFIG(self));
	self->submerged_scale = CLAMP(scale, 1, 4);
}

void
gowl_config_set_dew_intensity(GowlConfig *self, gdouble intensity)
{
	g_return_if_fail(GOWL_IS_CONFIG(self));
	self->dew_intensity = CLAMP(intensity, 0.0, 3.0);
}

void
gowl_config_set_dew_fps(GowlConfig *self, gint fps)
{
	g_return_if_fail(GOWL_IS_CONFIG(self));
	self->dew_fps = CLAMP(fps, 0, 144);
}

void
gowl_config_set_dew_scale(GowlConfig *self, gint scale)
{
	g_return_if_fail(GOWL_IS_CONFIG(self));
	self->dew_scale = CLAMP(scale, 1, 4);
}

/* --- Window hints (modules/hints) ---------------------------------- */

/**
 * gowl_config_hints_keys_valid:
 * @keys: a candidate alphabet
 *
 * Whether @keys can label windows.
 *
 * Two things disqualify it and both are worth refusing rather than
 * working around.  A REPEATED character would give two windows the same
 * label, so one of them could never be reached; and fewer than two
 * characters cannot make a two-character label either, so a config with
 * one key would silently cap the overlay at a single window.  Both look
 * like the overlay is broken rather than like the config is.
 *
 * Case-insensitive, because the labels are matched that way.
 *
 * Returns: %TRUE when @keys is usable as an alphabet.
 */
gboolean
gowl_config_hints_keys_valid(const gchar *keys)
{
	gboolean seen[128] = { FALSE };
	gsize i, n;

	if (keys == NULL)
		return FALSE;
	n = strlen(keys);
	if (n < 2)
		return FALSE;

	for (i = 0; i < n; i++) {
		guchar c = (guchar)g_ascii_tolower(keys[i]);

		/* Printable ASCII only: a label has to be a keysym somebody can
		 * press and the overlay matches against one byte. */
		if (c < 0x21 || c > 0x7e)
			return FALSE;
		if (seen[c])
			return FALSE;
		seen[c] = TRUE;
	}
	return TRUE;
}

const gchar *
gowl_config_get_hints_keys(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self),
	                     GOWL_CONFIG_DEFAULT_HINTS_KEYS);
	return self->hints_keys;
}

void
gowl_config_set_hints_keys(GowlConfig *self, const gchar *keys)
{
	g_return_if_fail(GOWL_IS_CONFIG(self));

	if (!gowl_config_hints_keys_valid(keys))
		return;
	g_free(self->hints_keys);
	self->hints_keys = g_ascii_strdown(keys, -1);
}

/**
 * gowl_config_get_no_fx_apps:
 * @self: a #GowlConfig
 *
 * The configured extra names that get no window effects, comma
 * separated.  Never %NULL; empty when nothing was configured, which is
 * the default -- the built-in names live in the matcher, not here.
 *
 * Returns: (transfer none): the list
 */
const gchar *
gowl_config_get_no_fx_apps(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self),
	                     GOWL_CONFIG_DEFAULT_NO_FX_APPS);
	return self->no_fx_apps != NULL ? self->no_fx_apps
	                                : GOWL_CONFIG_DEFAULT_NO_FX_APPS;
}

/**
 * gowl_config_set_no_fx_apps:
 * @self: a #GowlConfig
 * @apps: (nullable): comma-separated app_ids or process names; %NULL
 *   or the empty string leaves only the built-in names
 */
void
gowl_config_set_no_fx_apps(GowlConfig *self, const gchar *apps)
{
	g_return_if_fail(GOWL_IS_CONFIG(self));

	g_object_set(self, "no-fx-apps",
	             apps != NULL ? apps : GOWL_CONFIG_DEFAULT_NO_FX_APPS, NULL);
}

const gchar *
gowl_config_get_hints_colors(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self),
	                     GOWL_CONFIG_DEFAULT_HINTS_COLORS);
	return self->hints_colors;
}

void
gowl_config_set_hints_colors(GowlConfig *self, const gchar *colors)
{
	g_return_if_fail(GOWL_IS_CONFIG(self));

	if (colors == NULL || *colors == '\0')
		return;
	g_free(self->hints_colors);
	self->hints_colors = g_strdup(colors);
}

gint
gowl_config_get_hints_timeout(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self),
	                     GOWL_CONFIG_DEFAULT_HINTS_TIMEOUT);
	return self->hints_timeout;
}

void
gowl_config_set_hints_timeout(GowlConfig *self, gint ms)
{
	g_return_if_fail(GOWL_IS_CONFIG(self));
	self->hints_timeout = CLAMP(ms, 0, 60000);
}

gint
gowl_config_get_hints_size(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self),
	                     GOWL_CONFIG_DEFAULT_HINTS_SIZE);
	return self->hints_size;
}

gint
gowl_config_get_hints_border_width(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self),
	                     GOWL_CONFIG_DEFAULT_HINTS_BORDER_WIDTH);
	return self->hints_border_width;
}

gdouble
gowl_config_get_hints_scrim(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self),
	                     GOWL_CONFIG_DEFAULT_HINTS_SCRIM);
	return self->hints_scrim;
}

gboolean
gowl_config_get_hints_warp_pointer(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self),
	                     GOWL_CONFIG_DEFAULT_HINTS_WARP_POINTER);
	return self->hints_warp_pointer;
}

void
gowl_config_set_hints_warp_pointer(GowlConfig *self, gboolean warp)
{
	g_return_if_fail(GOWL_IS_CONFIG(self));
	self->hints_warp_pointer = warp;
}

gboolean
gowl_config_get_hints_current_output(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self),
	                     GOWL_CONFIG_DEFAULT_HINTS_CURRENT_OUTPUT);
	return self->hints_current_output;
}

gboolean
gowl_config_get_hdr_unmanaged(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self),
	                     GOWL_CONFIG_DEFAULT_HDR_UNMANAGED);
	return self->hdr_unmanaged;
}

gboolean
gowl_config_get_hdr_advertise_pq(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self),
	                     GOWL_CONFIG_DEFAULT_HDR_ADVERTISE_PQ);
	return self->hdr_advertise_pq;
}

gboolean
gowl_config_get_hdr_encode(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self),
	                     GOWL_CONFIG_DEFAULT_HDR_ENCODE);
	return self->hdr_encode;
}

void
gowl_config_set_hdr_encode(GowlConfig *self, gboolean encode)
{
	g_return_if_fail(GOWL_IS_CONFIG(self));
	self->hdr_encode = encode;
}

gint
gowl_config_get_hdr_bpc(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self),
	                     GOWL_CONFIG_DEFAULT_HDR_BPC);
	return self->hdr_bpc;
}

gdouble
gowl_config_get_hdr_sdr_white(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self),
	                     GOWL_CONFIG_DEFAULT_HDR_SDR_WHITE);
	return self->hdr_sdr_white;
}

void
gowl_config_set_hdr_sdr_white(GowlConfig *self, gdouble nits)
{
	g_return_if_fail(GOWL_IS_CONFIG(self));
	self->hdr_sdr_white = CLAMP(nits, 40.0, 600.0);
}

gint
gowl_config_get_water_fps(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self),
	                     GOWL_CONFIG_DEFAULT_WATER_FPS);
	return self->water_fps;
}

void
gowl_config_set_water_fps(GowlConfig *self, gint fps)
{
	g_return_if_fail(GOWL_IS_CONFIG(self));
	self->water_fps = CLAMP(fps, 0, 144);
}

gint
gowl_config_get_water_scale(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self),
	                     GOWL_CONFIG_DEFAULT_WATER_SCALE);
	return self->water_scale;
}

const gchar *
gowl_config_get_water_tint(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self),
	                     GOWL_CONFIG_DEFAULT_WATER_TINT);
	return self->water_tint;
}

gdouble
gowl_config_get_water_clarity(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self),
	                     GOWL_CONFIG_DEFAULT_WATER_CLARITY);
	return self->water_clarity;
}

gdouble
gowl_config_get_water_opacity(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self),
	                     GOWL_CONFIG_DEFAULT_WATER_OPACITY);
	return self->water_opacity;
}

gdouble
gowl_config_get_water_brightness(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self),
	                     GOWL_CONFIG_DEFAULT_WATER_BRIGHTNESS);
	return self->water_brightness;
}

gdouble
gowl_config_get_water_light(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self),
	                     GOWL_CONFIG_DEFAULT_WATER_LIGHT);
	return self->water_light;
}

gint
gowl_config_get_water_frost(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self),
	                     GOWL_CONFIG_DEFAULT_WATER_FROST);
	return self->water_frost;
}

gint
gowl_config_get_water_frost_passes(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self),
	                     GOWL_CONFIG_DEFAULT_WATER_FROST_PASSES);
	return self->water_frost_passes;
}

gboolean
gowl_config_get_shadow(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self), FALSE);
	return self->shadow;
}

gint
gowl_config_get_shadow_radius(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self),
	                     GOWL_CONFIG_DEFAULT_SHADOW_RADIUS);
	return self->shadow_radius;
}

gdouble
gowl_config_get_shadow_opacity(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self),
	                     GOWL_CONFIG_DEFAULT_SHADOW_OPACITY);
	return self->shadow_opacity;
}

gint
gowl_config_get_shadow_offset_x(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self), 0);
	return self->shadow_offset_x;
}

gint
gowl_config_get_shadow_offset_y(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self),
	                     GOWL_CONFIG_DEFAULT_SHADOW_OFFSET_Y);
	return self->shadow_offset_y;
}

const gchar *
gowl_config_get_shadow_color(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self),
	                     GOWL_CONFIG_DEFAULT_SHADOW_COLOR);
	return self->shadow_color;
}

/* --- Per-tag wallpaper --- */

const gchar *
gowl_config_get_wallpaper_for_tag(GowlConfig *self, gint tag)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self), NULL);

	if (tag < 1 || tag > GOWL_CONFIG_MAX_TAGS)
		return NULL;
	return self->wallpaper_tags[tag - 1];
}

gboolean
gowl_config_has_tag_wallpapers(GowlConfig *self)
{
	gint i;

	g_return_val_if_fail(GOWL_IS_CONFIG(self), FALSE);

	for (i = 0; i < GOWL_CONFIG_MAX_TAGS; i++) {
		if (self->wallpaper_tags[i] != NULL)
			return TRUE;
	}
	return FALSE;
}

gint
gowl_config_get_wallpaper_fade(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self),
	                     GOWL_CONFIG_DEFAULT_WALLPAPER_FADE);
	return self->wallpaper_fade;
}

/* --- Locking --- */

const gchar *
gowl_config_get_lock_command(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self), NULL);
	return self->lock_command;
}

void
gowl_config_set_lock_command(GowlConfig *self, const gchar *command)
{
	g_return_if_fail(GOWL_IS_CONFIG(self));

	g_free(self->lock_command);
	self->lock_command = g_strdup(command != NULL ? command : "");
}

gboolean
gowl_config_get_lock_on_suspend(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self),
	                     GOWL_CONFIG_DEFAULT_LOCK_ON_SUSPEND);
	return self->lock_on_suspend;
}

void
gowl_config_set_lock_on_suspend(GowlConfig *self, gboolean enable)
{
	g_return_if_fail(GOWL_IS_CONFIG(self));
	self->lock_on_suspend = enable != FALSE;
}

/* --- Per-output wallpaper --- */

static void
wallpaper_output_free(gpointer data)
{
	GowlWallpaperOutput *wo = (GowlWallpaperOutput *)data;

	if (wo == NULL)
		return;
	g_free(wo->path);
	g_free(wo->mode);
	g_free(wo);
}

/**
 * gowl_config_set_wallpaper_output:
 * @self: a #GowlConfig
 * @key: an output key, as `monitors:` takes them
 * @path: (nullable): the picture for that output, or %NULL to drop the entry
 * @mode: (nullable): a scaling mode for that output, or %NULL for the default
 *
 * Declares the wallpaper one output shows.  See the header.
 */
void
gowl_config_set_wallpaper_output(
	GowlConfig  *self,
	const gchar *key,
	const gchar *path,
	const gchar *mode
){
	GowlWallpaperOutput *wo;

	g_return_if_fail(GOWL_IS_CONFIG(self));
	g_return_if_fail(key != NULL && key[0] != '\0');

	if (path == NULL || path[0] == '\0') {
		if (self->wallpaper_outputs != NULL)
			g_hash_table_remove(self->wallpaper_outputs, key);
		return;
	}

	if (self->wallpaper_outputs == NULL)
		self->wallpaper_outputs = g_hash_table_new_full(
			g_str_hash, g_str_equal, g_free, wallpaper_output_free);

	wo = g_new0(GowlWallpaperOutput, 1);
	wo->path = g_strdup(path);
	wo->mode = (mode != NULL && mode[0] != '\0') ? g_strdup(mode) : NULL;
	g_hash_table_replace(self->wallpaper_outputs, g_strdup(key), wo);
}

/**
 * gowl_config_lookup_wallpaper_output:
 * @self: a #GowlConfig
 * @name: the connector name of an output
 * @make: (nullable): its make
 * @model: (nullable): its model
 * @serial: (nullable): its serial
 *
 * Returns: (transfer none) (nullable): the entry for this output
 */
const GowlWallpaperOutput *
gowl_config_lookup_wallpaper_output(
	GowlConfig  *self,
	const gchar *name,
	const gchar *make,
	const gchar *model,
	const gchar *serial
){
	g_return_val_if_fail(GOWL_IS_CONFIG(self), NULL);

	return (const GowlWallpaperOutput *)lookup_key_in(
		self->wallpaper_outputs, name, make, model, serial);
}

gboolean
gowl_config_has_output_wallpapers(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self), FALSE);

	return self->wallpaper_outputs != NULL
		&& g_hash_table_size(self->wallpaper_outputs) > 0;
}
