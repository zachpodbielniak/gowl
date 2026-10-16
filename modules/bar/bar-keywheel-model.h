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
 * The key wheel's model: everything about the widget that is not
 * painting, threads or the compositor.
 *
 * The widget snapshots the compositor's keybinds into a
 * #BarKeywheelModel and from then on asks only this file what goes
 * where: which ring a chord lives on, which face a key is drawn on,
 * where a cell sits on the canvas and which cell a click landed in,
 * what a key press in the panel means, and which chord Enter runs.
 * Keeping all of that pure is what lets the tests drive the wheel
 * without a compositor, a theme or a frame.
 *
 * Nothing here touches a #GowlBarPlugin, a theme object, a subprocess
 * or the compositor.  The modifier bits and colour roles are mirrored
 * as plain numbers; the tests pin them against gowl's own.
 */

#ifndef BAR_KEYWHEEL_MODEL_H
#define BAR_KEYWHEEL_MODEL_H

#include <glib.h>

G_BEGIN_DECLS

/* Mirrors of GowlKeyMod (gowl-enums.h); pinned by the tests. */
#define BAR_KEYWHEEL_MOD_SHIFT (1u << 0)
#define BAR_KEYWHEEL_MOD_CTRL  (1u << 2)
#define BAR_KEYWHEEL_MOD_ALT   (1u << 3)
#define BAR_KEYWHEEL_MOD_MOD3  (1u << 5)
#define BAR_KEYWHEEL_MOD_LOGO  (1u << 6)
#define BAR_KEYWHEEL_MOD_MOD5  (1u << 7)

/* Mirror of GOWL_KEYBIND_FLAG_RELEASE (gowl-config.h). */
#define BAR_KEYWHEEL_FLAG_RELEASE (1u << 1)

/* The most spokes a face carries.  Past this the cells are too thin to
   click; the rest stay reachable through search. */
#define BAR_KEYWHEEL_MAX_SPOKES  (40)
/* The Core face's spoke count, as in the original overlay. */
#define BAR_KEYWHEEL_CORE_SPOKES (12)
/* Bounds on what cmacs may push for the Emacs face. */
#define BAR_KEYWHEEL_MAX_EMACS   (300)
#define BAR_KEYWHEEL_FIELD_MAX   (256)
/* Search results shown at once. */
#define BAR_KEYWHEEL_MAX_RESULTS (12)

/**
 * BarKeywheelRing:
 * @BAR_KEYWHEEL_RING_SUPER: the outer rim, Super + key
 * @BAR_KEYWHEEL_RING_SHIFT: Super+Shift + key
 * @BAR_KEYWHEEL_RING_CTRL: Super+Ctrl + key
 * @BAR_KEYWHEEL_RING_HUB: Super+Shift+Ctrl + key, the innermost ring
 * @BAR_KEYWHEEL_RING_COUNT: sentinel
 *
 * The rings, outermost first.  Alt is not a ring: the whole wheel
 * flips to the same four rings with Alt added.
 */
typedef enum {
	BAR_KEYWHEEL_RING_SUPER = 0,
	BAR_KEYWHEEL_RING_SHIFT,
	BAR_KEYWHEEL_RING_CTRL,
	BAR_KEYWHEEL_RING_HUB,
	BAR_KEYWHEEL_RING_COUNT
} BarKeywheelRing;

/**
 * BarKeywheelFace:
 * @BAR_KEYWHEEL_FACE_CORE: the twelve keys that matter most
 * @BAR_KEYWHEEL_FACE_LETTERS: a to z
 * @BAR_KEYWHEEL_FACE_KEYS: digits, punctuation, arrows, F-keys, media
 * @BAR_KEYWHEEL_FACE_BEYOND: bindings without Super, as a list
 * @BAR_KEYWHEEL_FACE_EMACS: notable Emacs bindings cmacs pushed, a list;
 *   only present when there are some
 * @BAR_KEYWHEEL_FACE_COUNT: sentinel
 */
typedef enum {
	BAR_KEYWHEEL_FACE_CORE = 0,
	BAR_KEYWHEEL_FACE_LETTERS,
	BAR_KEYWHEEL_FACE_KEYS,
	BAR_KEYWHEEL_FACE_BEYOND,
	BAR_KEYWHEEL_FACE_EMACS,
	BAR_KEYWHEEL_FACE_COUNT
} BarKeywheelFace;

/**
 * BarKeywheelBind:
 * @modifiers: the bind's modifier bits, exactly as configured
 * @keysym: the bind's keysym, exactly as configured
 * @flags: the bind's #GowlKeybindFlags
 * @action: the action's name, e.g. "spawn"
 * @arg: (nullable): the action's argument
 * @desc: (nullable): the bind's description
 * @combo: the chord, spelled the way gowl spells it
 *
 * One keybind, copied out of the compositor's config.  Nothing here
 * points back into the compositor, so a config reload cannot pull it
 * out from under the panel.
 */
typedef struct {
	guint  modifiers;
	guint  keysym;
	guint  flags;
	gchar *action;
	gchar *arg;
	gchar *desc;
	gchar *combo;
} BarKeywheelBind;

/**
 * BarKeywheelSpoke:
 * @label: what the cell says
 * @keysyms: the keys the spoke stands for, lower-cased
 * @n_keysyms: how many; more than one only for the Core face's 1--9
 *
 * One spoke of a face: a key, or (on Core) a group of keys that do the
 * same thing to different tags.
 */
typedef struct {
	gchar *label;
	guint  keysyms[10];
	guint  n_keysyms;
} BarKeywheelSpoke;

/**
 * BarKeywheelEmacs:
 * @key: the key sequence, as `key-description' prints it
 * @command: the command's name
 * @doc: the first line of its documentation
 */
typedef struct {
	gchar *key;
	gchar *command;
	gchar *doc;
} BarKeywheelEmacs;

/**
 * BarKeywheelModel:
 * @binds: (element-type BarKeywheelBind): every snapshot bind
 * @spokes: (element-type BarKeywheelSpoke): one array per wheel face
 * @beyond: (element-type guint): indices into @binds without Super
 * @emacs: (element-type BarKeywheelEmacs) (nullable): cmacs's list
 * @keys_overflow: how many keys did not fit on the Keys face
 */
typedef struct {
	GPtrArray *binds;
	GPtrArray *spokes[3];
	GArray    *beyond;
	GPtrArray *emacs;
	guint      keys_overflow;
} BarKeywheelModel;

/**
 * BarKeywheelCursor:
 * @face: the face showing
 * @spoke: the selected spoke, or -1
 * @ring: the selected ring
 * @alt: whether the wheel is flipped to its Alt rings
 * @keysym: the concrete key selected, for a spoke standing for several
 * @row: on a list face, the selected row (an index into the list), or -1
 */
typedef struct {
	BarKeywheelFace face;
	gint            spoke;
	BarKeywheelRing ring;
	gboolean        alt;
	guint           keysym;
	gint            row;
} BarKeywheelCursor;

/**
 * BarKeywheelKeyResult:
 * @BAR_KEYWHEEL_KEY_IGNORED: not the wheel's; let the host have it
 * @BAR_KEYWHEEL_KEY_CLAIMED: the wheel's, but nothing changed
 * @BAR_KEYWHEEL_KEY_MOVED: the cursor changed; repaint
 * @BAR_KEYWHEEL_KEY_SEARCH: open the search field
 */
typedef enum {
	BAR_KEYWHEEL_KEY_IGNORED = 0,
	BAR_KEYWHEEL_KEY_CLAIMED,
	BAR_KEYWHEEL_KEY_MOVED,
	BAR_KEYWHEEL_KEY_SEARCH
} BarKeywheelKeyResult;

/**
 * BarKeywheelHit:
 * @BAR_KEYWHEEL_HIT_NONE: nothing
 * @BAR_KEYWHEEL_HIT_CELL: a ring cell
 * @BAR_KEYWHEEL_HIT_DOT: a face dot
 * @BAR_KEYWHEEL_HIT_ALT: the Alt switch (the hub's centre, or the pill)
 */
typedef enum {
	BAR_KEYWHEEL_HIT_NONE = 0,
	BAR_KEYWHEEL_HIT_CELL,
	BAR_KEYWHEEL_HIT_DOT,
	BAR_KEYWHEEL_HIT_ALT
} BarKeywheelHit;

/**
 * BarKeywheelColorKind:
 * @BAR_KEYWHEEL_COLOR_FALLBACK: unset or unusable; use the default role
 * @BAR_KEYWHEEL_COLOR_ROLE: a palette role name
 * @BAR_KEYWHEEL_COLOR_HEX: a `#rrggbb' or `#rrggbbaa' literal
 */
typedef enum {
	BAR_KEYWHEEL_COLOR_FALLBACK = 0,
	BAR_KEYWHEEL_COLOR_ROLE,
	BAR_KEYWHEEL_COLOR_HEX
} BarKeywheelColorKind;

/**
 * BarKeywheelGeom:
 *
 * Where everything on the canvas sits, for one canvas size and face.
 * Computed by bar_keywheel_geom(); the painter and the hit test both
 * read it, so a cell is clickable exactly where it is drawn.
 */
typedef struct {
	gdouble  width, height;
	gboolean wheel;
	guint    n_spokes;
	guint    n_faces;
	gdouble  header_h;
	gdouble  cx, cy, radius;
	gdouble  hub_r;
	gdouble  ring_in[BAR_KEYWHEEL_RING_COUNT];
	gdouble  ring_out[BAR_KEYWHEEL_RING_COUNT];
	gdouble  dots_y, dot_x0, dot_step, dot_r;
	gdouble  alt_x, alt_y, alt_w, alt_h;
	gdouble  details_x, details_y, details_w, details_h;
} BarKeywheelGeom;

/* --- Chords --------------------------------------------------------- */

gboolean     bar_keywheel_classify        (guint             modifiers,
                                            BarKeywheelRing  *ring,
                                            gboolean         *alt);
guint        bar_keywheel_ring_mods       (BarKeywheelRing   ring,
                                            gboolean          alt);
guint        bar_keywheel_normalize_keysym (guint            keysym,
                                            guint            modifiers);
gchar       *bar_keywheel_key_label       (guint             keysym);
gchar       *bar_keywheel_combo           (guint             modifiers,
                                            guint             keysym);

/* --- The model ------------------------------------------------------ */

BarKeywheelModel *bar_keywheel_model_new  (void);
void              bar_keywheel_model_free (BarKeywheelModel *model);
void              bar_keywheel_model_add_bind (BarKeywheelModel *model,
                                               guint             modifiers,
                                               guint             keysym,
                                               guint             flags,
                                               const gchar      *action,
                                               const gchar      *arg,
                                               const gchar      *desc);
void              bar_keywheel_model_finish   (BarKeywheelModel *model);
void              bar_keywheel_model_set_emacs (BarKeywheelModel *model,
                                                GPtrArray        *emacs);

guint        bar_keywheel_face_count     (const BarKeywheelModel *model);
gboolean     bar_keywheel_face_is_wheel  (BarKeywheelFace face);
const gchar *bar_keywheel_face_title     (BarKeywheelFace face);
guint        bar_keywheel_n_spokes       (const BarKeywheelModel *model,
                                          BarKeywheelFace         face);
const BarKeywheelSpoke *bar_keywheel_spoke (const BarKeywheelModel *model,
                                            BarKeywheelFace         face,
                                            guint                   index);
gint         bar_keywheel_find_spoke     (const BarKeywheelModel *model,
                                          BarKeywheelFace         face,
                                          guint                   keysym);
const BarKeywheelBind *bar_keywheel_cell (const BarKeywheelModel *model,
                                          BarKeywheelFace         face,
                                          guint                   spoke,
                                          BarKeywheelRing         ring,
                                          gboolean                alt,
                                          guint                   prefer);
const BarKeywheelBind *bar_keywheel_bind (const BarKeywheelModel *model,
                                          guint                   index);
guint        bar_keywheel_list_len       (const BarKeywheelModel *model,
                                          BarKeywheelFace         face);

/* --- The cursor ----------------------------------------------------- */

void                 bar_keywheel_cursor_reset (BarKeywheelCursor *cursor);
void                 bar_keywheel_cursor_face  (const BarKeywheelModel *model,
                                                BarKeywheelCursor      *cursor,
                                                gint                    step);
BarKeywheelKeyResult bar_keywheel_handle_key   (const BarKeywheelModel *model,
                                                BarKeywheelCursor      *cursor,
                                                guint                   keysym,
                                                guint                   modifiers);
gboolean             bar_keywheel_select_cell  (const BarKeywheelModel *model,
                                                BarKeywheelCursor      *cursor,
                                                guint                   spoke,
                                                BarKeywheelRing         ring);
gboolean             bar_keywheel_run_target   (const BarKeywheelModel *model,
                                                const BarKeywheelCursor *cursor,
                                                guint                  *modifiers,
                                                guint                  *keysym,
                                                guint                  *flags);

/* --- Search --------------------------------------------------------- */

guint bar_keywheel_search (const BarKeywheelModel *model,
                           const gchar            *query,
                           guint                   max,
                           GArray                 *binds_out,
                           GArray                 *emacs_out);

/* --- The Emacs face ------------------------------------------------- */

gchar            *bar_keywheel_unescape     (const gchar *text);
BarKeywheelEmacs *bar_keywheel_emacs_parse  (const gchar *line);
void              bar_keywheel_emacs_free   (BarKeywheelEmacs *entry);

/* --- Settings ------------------------------------------------------- */

void                 bar_keywheel_parse_size     (const gchar *spec,
                                                  gint        *width,
                                                  gint        *height);
gboolean             bar_keywheel_role_from_name (const gchar *name,
                                                  gint        *role);
const gchar         *bar_keywheel_role_name      (gint         role);
BarKeywheelColorKind bar_keywheel_color_resolve  (const gchar *spec,
                                                  gint         fallback,
                                                  gint        *role,
                                                  gdouble      rgba[4]);

/* --- Geometry ------------------------------------------------------- */

void           bar_keywheel_geom        (gdouble          width,
                                         gdouble          height,
                                         guint            n_spokes,
                                         guint            n_faces,
                                         gboolean         wheel,
                                         BarKeywheelGeom *geom);
void           bar_keywheel_spoke_span  (const BarKeywheelGeom *geom,
                                         guint                  spoke,
                                         gdouble               *a0,
                                         gdouble               *a1);
void           bar_keywheel_cell_center (const BarKeywheelGeom *geom,
                                         guint                  spoke,
                                         BarKeywheelRing        ring,
                                         gdouble               *x,
                                         gdouble               *y);
void           bar_keywheel_dot_center  (const BarKeywheelGeom *geom,
                                         guint                  face,
                                         gdouble               *x,
                                         gdouble               *y);
BarKeywheelHit bar_keywheel_hit         (const BarKeywheelGeom *geom,
                                         gdouble                x,
                                         gdouble                y,
                                         gint                  *spoke,
                                         BarKeywheelRing       *ring,
                                         gint                  *face);

/* --- The widget (bar-plugins-keywheel.c) ------------------------------ */

/* The one way a model reaches the widget's instance data; takes
   ownership of @model.  Documented at its definition. */
void bar_keywheel_widget_install (gpointer          data,
                                  BarKeywheelModel *model);

G_END_DECLS

#endif /* BAR_KEYWHEEL_MODEL_H */
