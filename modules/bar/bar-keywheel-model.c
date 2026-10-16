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
 * The key wheel's pure half.  See bar-keywheel-model.h for what lives
 * here and why; docs/bar-widgets/keywheel.org for what it looks like.
 */

#include "bar-keywheel-model.h"

#include <math.h>
#include <string.h>
#include <xkbcommon/xkbcommon.h>

/* Modifiers that decide where a chord is drawn.  NumLock (Mod2) and
   CapsLock are not among them, for the same reason the compositor's
   own match ignores them. */
#define KW_MOD_MASK (BAR_KEYWHEEL_MOD_SHIFT | BAR_KEYWHEEL_MOD_CTRL | \
                     BAR_KEYWHEEL_MOD_ALT | BAR_KEYWHEEL_MOD_LOGO | \
                     BAR_KEYWHEEL_MOD_MOD3 | BAR_KEYWHEEL_MOD_MOD5)

/* ----------------------------------------------------------------
 * Chords
 * ---------------------------------------------------------------- */

/**
 * bar_keywheel_classify:
 * @modifiers: a bind's modifier bits
 * @ring: (out) (optional): the ring the chord is drawn on
 * @alt: (out) (optional): whether it is on the Alt flip
 *
 * Where a chord lives.  Every Super chord built only from Shift, Ctrl
 * and Alt is on the wheel; anything else -- no Super, or a Mod3/Mod5
 * layer -- is "beyond Super" and goes in the list.
 *
 * Returns: %TRUE when the chord is on the wheel
 */
gboolean
bar_keywheel_classify(guint modifiers, BarKeywheelRing *ring, gboolean *alt)
{
	guint m = modifiers & KW_MOD_MASK;
	gboolean shift, ctrl;
	BarKeywheelRing r;

	if ((m & BAR_KEYWHEEL_MOD_LOGO) == 0 ||
	    (m & (BAR_KEYWHEEL_MOD_MOD3 | BAR_KEYWHEEL_MOD_MOD5)) != 0)
		return FALSE;

	shift = (m & BAR_KEYWHEEL_MOD_SHIFT) != 0;
	ctrl  = (m & BAR_KEYWHEEL_MOD_CTRL) != 0;
	if (shift && ctrl)
		r = BAR_KEYWHEEL_RING_HUB;
	else if (shift)
		r = BAR_KEYWHEEL_RING_SHIFT;
	else if (ctrl)
		r = BAR_KEYWHEEL_RING_CTRL;
	else
		r = BAR_KEYWHEEL_RING_SUPER;

	if (ring != NULL)
		*ring = r;
	if (alt != NULL)
		*alt = (m & BAR_KEYWHEEL_MOD_ALT) != 0;
	return TRUE;
}

/**
 * bar_keywheel_ring_mods:
 * @ring: a ring
 * @alt: the Alt flip
 *
 * The inverse of bar_keywheel_classify().
 *
 * Returns: the modifier bits a chord on @ring has
 */
guint
bar_keywheel_ring_mods(BarKeywheelRing ring, gboolean alt)
{
	guint m = BAR_KEYWHEEL_MOD_LOGO;

	switch (ring) {
	case BAR_KEYWHEEL_RING_SHIFT:
		m |= BAR_KEYWHEEL_MOD_SHIFT;
		break;
	case BAR_KEYWHEEL_RING_CTRL:
		m |= BAR_KEYWHEEL_MOD_CTRL;
		break;
	case BAR_KEYWHEEL_RING_HUB:
		m |= BAR_KEYWHEEL_MOD_SHIFT | BAR_KEYWHEEL_MOD_CTRL;
		break;
	case BAR_KEYWHEEL_RING_SUPER:
	case BAR_KEYWHEEL_RING_COUNT:
	default:
		break;
	}
	if (alt)
		m |= BAR_KEYWHEEL_MOD_ALT;
	return m;
}

/*
 * The unshifted key behind a shifted symbol, on a US layout.
 *
 * A config names the key ("Super+Shift+slash"), but a key press with
 * Shift held arrives as the symbol it types (`question').  To land the
 * press on the spoke it means, the symbol has to be turned back into
 * its key.  The real answer lives in the keymap, which the panel does
 * not have; the US table is right for the layouts gowl ships and only
 * costs a non-US user a click on an arrow key.
 */
static const struct {
	guint shifted;
	guint plain;
} kw_unshift[] = {
	{ XKB_KEY_exclam,      XKB_KEY_1 },
	{ XKB_KEY_at,          XKB_KEY_2 },
	{ XKB_KEY_numbersign,  XKB_KEY_3 },
	{ XKB_KEY_dollar,      XKB_KEY_4 },
	{ XKB_KEY_percent,     XKB_KEY_5 },
	{ XKB_KEY_asciicircum, XKB_KEY_6 },
	{ XKB_KEY_ampersand,   XKB_KEY_7 },
	{ XKB_KEY_asterisk,    XKB_KEY_8 },
	{ XKB_KEY_parenleft,   XKB_KEY_9 },
	{ XKB_KEY_parenright,  XKB_KEY_0 },
	{ XKB_KEY_underscore,  XKB_KEY_minus },
	{ XKB_KEY_plus,        XKB_KEY_equal },
	{ XKB_KEY_braceleft,   XKB_KEY_bracketleft },
	{ XKB_KEY_braceright,  XKB_KEY_bracketright },
	{ XKB_KEY_bar,         XKB_KEY_backslash },
	{ XKB_KEY_colon,       XKB_KEY_semicolon },
	{ XKB_KEY_quotedbl,    XKB_KEY_apostrophe },
	{ XKB_KEY_asciitilde,  XKB_KEY_grave },
	{ XKB_KEY_less,        XKB_KEY_comma },
	{ XKB_KEY_greater,     XKB_KEY_period },
	{ XKB_KEY_question,    XKB_KEY_slash },
	{ XKB_KEY_ISO_Left_Tab, XKB_KEY_Tab }
};

/**
 * bar_keywheel_normalize_keysym:
 * @keysym: a keysym, as configured or as pressed
 * @modifiers: the modifiers it came with
 *
 * The key a spoke is filed under: letters lower-cased, and (with Shift
 * held) a shifted symbol turned back into its key.  Both the snapshot
 * and the key handler go through this, so `Super+Shift+Q' in a config
 * and a press of Shift+q in the panel meet on the same spoke.
 *
 * Returns: the normalised keysym
 */
guint
bar_keywheel_normalize_keysym(guint keysym, guint modifiers)
{
	guint i;

	(void)modifiers;
	for (i = 0; i < G_N_ELEMENTS(kw_unshift); i++)
		if (kw_unshift[i].shifted == keysym)
			return kw_unshift[i].plain;
	return (guint)xkb_keysym_to_lower((xkb_keysym_t)keysym);
}

/* Short names for the keys whose keysym names are too long for a cell
   or mean nothing to a reader (`Prior'). */
static const struct {
	guint        keysym;
	const gchar *label;
} kw_labels[] = {
	{ XKB_KEY_Return,       "Ret" },
	{ XKB_KEY_KP_Enter,     "Ent" },
	{ XKB_KEY_space,        "Spc" },
	{ XKB_KEY_Tab,          "Tab" },
	{ XKB_KEY_Escape,       "Esc" },
	{ XKB_KEY_BackSpace,    "Bksp" },
	{ XKB_KEY_Delete,       "Del" },
	{ XKB_KEY_Insert,       "Ins" },
	{ XKB_KEY_Home,         "Home" },
	{ XKB_KEY_End,          "End" },
	{ XKB_KEY_Prior,        "PgUp" },
	{ XKB_KEY_Next,         "PgDn" },
	{ XKB_KEY_Left,         "\xe2\x86\x90" },
	{ XKB_KEY_Up,           "\xe2\x86\x91" },
	{ XKB_KEY_Right,        "\xe2\x86\x92" },
	{ XKB_KEY_Down,         "\xe2\x86\x93" },
	{ XKB_KEY_Print,        "Prt" },
	{ XKB_KEY_comma,        "," },
	{ XKB_KEY_period,       "." },
	{ XKB_KEY_slash,        "/" },
	{ XKB_KEY_backslash,    "\\" },
	{ XKB_KEY_semicolon,    ";" },
	{ XKB_KEY_apostrophe,   "'" },
	{ XKB_KEY_grave,        "`" },
	{ XKB_KEY_minus,        "-" },
	{ XKB_KEY_equal,        "=" },
	{ XKB_KEY_bracketleft,  "[" },
	{ XKB_KEY_bracketright, "]" },
	{ XKB_KEY_XF86AudioRaiseVolume,  "Vol+" },
	{ XKB_KEY_XF86AudioLowerVolume,  "Vol-" },
	{ XKB_KEY_XF86AudioMute,         "Mute" },
	{ XKB_KEY_XF86AudioMicMute,      "Mic" },
	{ XKB_KEY_XF86MonBrightnessUp,   "Bri+" },
	{ XKB_KEY_XF86MonBrightnessDown, "Bri-" },
	{ XKB_KEY_XF86AudioPlay,         "Play" },
	{ XKB_KEY_XF86AudioPause,        "Pause" },
	{ XKB_KEY_XF86AudioNext,         "Next" },
	{ XKB_KEY_XF86AudioPrev,         "Prev" },
	{ XKB_KEY_XF86AudioStop,         "Stop" }
};

/**
 * bar_keywheel_key_label:
 * @keysym: a keysym
 *
 * What a cell says for @keysym: a short name for the common named
 * keys, the keysym's own name otherwise, never longer than six
 * characters so it fits the rim.
 *
 * Returns: (transfer full): the label
 */
gchar *
bar_keywheel_key_label(guint keysym)
{
	gchar name[64];
	const gchar *src;
	guint i;
	gint n;

	for (i = 0; i < G_N_ELEMENTS(kw_labels); i++)
		if (kw_labels[i].keysym == keysym)
			return g_strdup(kw_labels[i].label);

	n = xkb_keysym_get_name((xkb_keysym_t)keysym, name, sizeof(name));
	if (n <= 0)
		return g_strdup("?");

	/* `XF86Calculator' -> `Calcul': the prefix is on every media key
	   and says nothing. */
	src = name;
	if (g_str_has_prefix(src, "XF86"))
		src += 4;
	if (strlen(src) <= 6)
		return g_strdup(src);
	return g_strndup(src, 6);
}

/**
 * bar_keywheel_combo:
 * @modifiers: modifier bits
 * @keysym: a keysym
 *
 * The chord spelled as gowl_keybind_to_string() spells it (Super, Ctrl,
 * Alt, Shift, then the key's name), so what the wheel shows can be
 * pasted straight into a config.
 *
 * Returns: (transfer full): e.g. "Super+Shift+Return"
 */
gchar *
bar_keywheel_combo(guint modifiers, guint keysym)
{
	GString *out;
	gchar name[64];

	out = g_string_new(NULL);
	if (modifiers & BAR_KEYWHEEL_MOD_LOGO)
		g_string_append(out, "Super+");
	if (modifiers & BAR_KEYWHEEL_MOD_CTRL)
		g_string_append(out, "Ctrl+");
	if (modifiers & BAR_KEYWHEEL_MOD_ALT)
		g_string_append(out, "Alt+");
	if (modifiers & BAR_KEYWHEEL_MOD_SHIFT)
		g_string_append(out, "Shift+");
	if (modifiers & BAR_KEYWHEEL_MOD_MOD3)
		g_string_append(out, "Mod3+");
	if (modifiers & BAR_KEYWHEEL_MOD_MOD5)
		g_string_append(out, "Mod5+");
	if (xkb_keysym_get_name((xkb_keysym_t)keysym, name, sizeof(name)) > 0)
		g_string_append(out, name);
	else
		g_string_append(out, "NoSymbol");
	return g_string_free(out, FALSE);
}

/* ----------------------------------------------------------------
 * The model
 * ---------------------------------------------------------------- */

static void
kw_bind_free(gpointer data)
{
	BarKeywheelBind *b = data;

	if (b == NULL)
		return;
	g_free(b->action);
	g_free(b->arg);
	g_free(b->desc);
	g_free(b->combo);
	g_free(b);
}

static void
kw_spoke_free(gpointer data)
{
	BarKeywheelSpoke *s = data;

	if (s == NULL)
		return;
	g_free(s->label);
	g_free(s);
}

/*
 * Text from the config is the user's, but it is still text from
 * outside: it reaches pango, so it must be UTF-8, and it is drawn in a
 * fixed box, so it is bounded.  Truncation is on a character boundary.
 */
static gchar *
kw_clean(const gchar *text)
{
	g_autofree gchar *valid = NULL;
	const gchar *end;

	if (text == NULL || text[0] == '\0')
		return NULL;
	valid = g_utf8_make_valid(text, -1);
	if (strlen(valid) <= BAR_KEYWHEEL_FIELD_MAX)
		return g_steal_pointer(&valid);
	end = g_utf8_find_prev_char(valid, valid + BAR_KEYWHEEL_FIELD_MAX);
	if (end == NULL)
		end = valid;
	return g_strndup(valid, (gsize)(end - valid));
}

/**
 * bar_keywheel_model_new:
 *
 * Returns: (transfer full): an empty model; add binds, then finish it
 */
BarKeywheelModel *
bar_keywheel_model_new(void)
{
	BarKeywheelModel *model;
	guint i;

	model = g_new0(BarKeywheelModel, 1);
	model->binds = g_ptr_array_new_with_free_func(kw_bind_free);
	for (i = 0; i < G_N_ELEMENTS(model->spokes); i++)
		model->spokes[i] = g_ptr_array_new_with_free_func(
			kw_spoke_free);
	model->beyond = g_array_new(FALSE, FALSE, sizeof(guint));
	return model;
}

/**
 * bar_keywheel_model_free:
 * @model: (nullable) (transfer full): a model
 */
void
bar_keywheel_model_free(BarKeywheelModel *model)
{
	guint i;

	if (model == NULL)
		return;
	g_ptr_array_unref(model->binds);
	for (i = 0; i < G_N_ELEMENTS(model->spokes); i++)
		g_ptr_array_unref(model->spokes[i]);
	g_array_unref(model->beyond);
	if (model->emacs != NULL)
		g_ptr_array_unref(model->emacs);
	g_free(model);
}

/**
 * bar_keywheel_model_add_bind:
 * @model: a model
 * @modifiers: the bind's modifiers, as configured
 * @keysym: the bind's keysym, as configured
 * @flags: its #GowlKeybindFlags
 * @action: (nullable): the action's name
 * @arg: (nullable): its argument
 * @desc: (nullable): its description
 *
 * Copies one bind in.  The modifiers and keysym are kept exactly as
 * configured, because they are what Enter hands back to the
 * compositor's dispatch, and that matches on the configured values.
 */
void
bar_keywheel_model_add_bind(BarKeywheelModel *model, guint modifiers,
                            guint keysym, guint flags, const gchar *action,
                            const gchar *arg, const gchar *desc)
{
	BarKeywheelBind *b;

	g_return_if_fail(model != NULL);

	b = g_new0(BarKeywheelBind, 1);
	b->modifiers = modifiers;
	b->keysym    = keysym;
	b->flags     = flags;
	b->action    = kw_clean(action);
	if (b->action == NULL)
		b->action = g_strdup("none");
	b->arg   = kw_clean(arg);
	b->desc  = kw_clean(desc);
	b->combo = bar_keywheel_combo(modifiers, keysym);
	g_ptr_array_add(model->binds, b);
}

/* Is any wheel chord bound on @keysym (normalised), in any ring? */
static gboolean
kw_key_bound(const BarKeywheelModel *model, guint keysym)
{
	guint i;

	for (i = 0; i < model->binds->len; i++) {
		const BarKeywheelBind *b = g_ptr_array_index(model->binds, i);

		if (bar_keywheel_classify(b->modifiers, NULL, NULL) &&
		    bar_keywheel_normalize_keysym(b->keysym, b->modifiers)
		    == keysym)
			return TRUE;
	}
	return FALSE;
}

static BarKeywheelSpoke *
kw_spoke_new(guint keysym)
{
	BarKeywheelSpoke *s = g_new0(BarKeywheelSpoke, 1);

	s->label = bar_keywheel_key_label(keysym);
	s->keysyms[0] = keysym;
	s->n_keysyms = 1;
	return s;
}

/*
 * The Core face.  The keys a newcomer reaches for first, in the order
 * the original overlay put them: the terminal, closing, the launcher,
 * the four movement keys, the tags, layouts, fullscreen, the run box.
 * `1-9' is ONE spoke: nine cells saying "tag N" are one idea.
 *
 * Only keys that are actually bound get a spoke; when fewer than
 * twelve of the first choices are, the next most likely keys fill in,
 * so a config that moved things around still gets a full Core face.
 */
#define KW_DIGITS ((guint)0xFFFFFFFFu)

static const guint kw_core_pref[] = {
	XKB_KEY_Return, XKB_KEY_q, XKB_KEY_c, XKB_KEY_space,
	XKB_KEY_j, XKB_KEY_k, XKB_KEY_h, XKB_KEY_l,
	KW_DIGITS, XKB_KEY_Tab, XKB_KEY_f, XKB_KEY_p,
	/* Fillers, most likely first. */
	XKB_KEY_m, XKB_KEY_t, XKB_KEY_w, XKB_KEY_s, XKB_KEY_v,
	XKB_KEY_r, XKB_KEY_d, XKB_KEY_i, XKB_KEY_n, XKB_KEY_e,
	XKB_KEY_b, XKB_KEY_g, XKB_KEY_comma, XKB_KEY_period,
	XKB_KEY_slash, XKB_KEY_Escape, XKB_KEY_0
};

static void
kw_build_core(BarKeywheelModel *model)
{
	guint i, d;

	for (i = 0; i < G_N_ELEMENTS(kw_core_pref) &&
	            model->spokes[0]->len < BAR_KEYWHEEL_CORE_SPOKES; i++) {
		BarKeywheelSpoke *s;

		if (kw_core_pref[i] != KW_DIGITS) {
			if (kw_key_bound(model, kw_core_pref[i]))
				g_ptr_array_add(model->spokes[0],
				                kw_spoke_new(kw_core_pref[i]));
			continue;
		}

		s = g_new0(BarKeywheelSpoke, 1);
		for (d = 0; d < 9; d++)
			if (kw_key_bound(model, XKB_KEY_1 + d))
				s->keysyms[s->n_keysyms++] = XKB_KEY_1 + d;
		if (s->n_keysyms == 0) {
			kw_spoke_free(s);
			continue;
		}
		s->label = g_strdup("1-9");
		g_ptr_array_add(model->spokes[0], s);
	}
}

/* Where a non-letter key sorts on the Keys face: digits in keyboard
   order, then the named keys, punctuation, F-keys, and the rest. */
static const guint kw_named_order[] = {
	XKB_KEY_Return, XKB_KEY_Tab, XKB_KEY_space, XKB_KEY_Escape,
	XKB_KEY_BackSpace, XKB_KEY_Delete, XKB_KEY_Insert, XKB_KEY_Home,
	XKB_KEY_End, XKB_KEY_Prior, XKB_KEY_Next, XKB_KEY_Left,
	XKB_KEY_Up, XKB_KEY_Right, XKB_KEY_Down, XKB_KEY_Print
};

static void
kw_key_rank(guint keysym, guint *category, guint *rank)
{
	guint i;

	if (keysym >= XKB_KEY_0 && keysym <= XKB_KEY_9) {
		*category = 0;
		*rank = (keysym == XKB_KEY_0) ? 10 : keysym - XKB_KEY_0;
		return;
	}
	for (i = 0; i < G_N_ELEMENTS(kw_named_order); i++) {
		if (kw_named_order[i] == keysym) {
			*category = 1;
			*rank = i;
			return;
		}
	}
	if (keysym > XKB_KEY_space && keysym <= XKB_KEY_asciitilde) {
		*category = 2;
		*rank = keysym;
		return;
	}
	if (keysym >= XKB_KEY_F1 && keysym <= XKB_KEY_F35) {
		*category = 3;
		*rank = keysym;
		return;
	}
	*category = 4;
	*rank = keysym;
}

static gint
kw_cmp_keys(gconstpointer a, gconstpointer b)
{
	guint ka = *(const guint *)a, kb = *(const guint *)b;
	guint ca, ra, cb, rb;

	kw_key_rank(ka, &ca, &ra);
	kw_key_rank(kb, &cb, &rb);
	if (ca != cb)
		return (ca < cb) ? -1 : 1;
	if (ra != rb)
		return (ra < rb) ? -1 : 1;
	return 0;
}

static gboolean
kw_is_letter(guint keysym)
{
	return keysym >= XKB_KEY_a && keysym <= XKB_KEY_z;
}

/**
 * bar_keywheel_model_finish:
 * @model: a model with every bind added
 *
 * Lays the binds out into faces.  Call once, after the last
 * bar_keywheel_model_add_bind().
 */
void
bar_keywheel_model_finish(BarKeywheelModel *model)
{
	g_autoptr(GArray) keys = NULL;
	guint i, k;

	g_return_if_fail(model != NULL);

	/* Letters: always the whole alphabet, bound or not.  A cheat
	   sheet that hides the free keys cannot answer "what is free?". */
	for (k = XKB_KEY_a; k <= XKB_KEY_z; k++)
		g_ptr_array_add(model->spokes[1], kw_spoke_new(k));

	keys = g_array_new(FALSE, FALSE, sizeof(guint));
	for (i = 0; i < model->binds->len; i++) {
		const BarKeywheelBind *b = g_ptr_array_index(model->binds, i);
		guint key, j;
		gboolean seen = FALSE;

		if (!bar_keywheel_classify(b->modifiers, NULL, NULL)) {
			g_array_append_val(model->beyond, i);
			continue;
		}
		key = bar_keywheel_normalize_keysym(b->keysym, b->modifiers);
		if (kw_is_letter(key))
			continue;
		for (j = 0; j < keys->len; j++)
			if (g_array_index(keys, guint, j) == key)
				seen = TRUE;
		if (!seen)
			g_array_append_val(keys, key);
	}

	g_array_sort(keys, kw_cmp_keys);
	for (i = 0; i < keys->len; i++) {
		if (i >= BAR_KEYWHEEL_MAX_SPOKES) {
			model->keys_overflow = keys->len - i;
			break;
		}
		g_ptr_array_add(model->spokes[2],
		                kw_spoke_new(g_array_index(keys, guint, i)));
	}

	kw_build_core(model);
}

/**
 * bar_keywheel_model_set_emacs:
 * @model: a model
 * @emacs: (nullable) (element-type BarKeywheelEmacs): the Emacs face's
 *   entries; referenced, not copied
 */
void
bar_keywheel_model_set_emacs(BarKeywheelModel *model, GPtrArray *emacs)
{
	g_return_if_fail(model != NULL);

	if (emacs != NULL)
		g_ptr_array_ref(emacs);
	if (model->emacs != NULL)
		g_ptr_array_unref(model->emacs);
	model->emacs = emacs;
}

/**
 * bar_keywheel_face_count:
 * @model: (nullable): a model
 *
 * Returns: 5 when cmacs pushed an Emacs face, otherwise 4
 */
guint
bar_keywheel_face_count(const BarKeywheelModel *model)
{
	if (model != NULL && model->emacs != NULL && model->emacs->len > 0)
		return BAR_KEYWHEEL_FACE_COUNT;
	return BAR_KEYWHEEL_FACE_EMACS;
}

/**
 * bar_keywheel_face_is_wheel:
 * @face: a face
 *
 * Returns: %TRUE for the three faces drawn as a wheel
 */
gboolean
bar_keywheel_face_is_wheel(BarKeywheelFace face)
{
	return face == BAR_KEYWHEEL_FACE_CORE ||
	       face == BAR_KEYWHEEL_FACE_LETTERS ||
	       face == BAR_KEYWHEEL_FACE_KEYS;
}

/**
 * bar_keywheel_face_title:
 * @face: a face
 *
 * Returns: (transfer none): its title
 */
const gchar *
bar_keywheel_face_title(BarKeywheelFace face)
{
	switch (face) {
	case BAR_KEYWHEEL_FACE_CORE:    return "Core";
	case BAR_KEYWHEEL_FACE_LETTERS: return "Letters";
	case BAR_KEYWHEEL_FACE_KEYS:    return "Keys & symbols";
	case BAR_KEYWHEEL_FACE_BEYOND:  return "Beyond Super";
	case BAR_KEYWHEEL_FACE_EMACS:   return "Emacs";
	case BAR_KEYWHEEL_FACE_COUNT:
	default:                        return "";
	}
}

/**
 * bar_keywheel_n_spokes:
 * @model: (nullable): a model
 * @face: a face
 *
 * Returns: the spokes on @face; 0 for a list face
 */
guint
bar_keywheel_n_spokes(const BarKeywheelModel *model, BarKeywheelFace face)
{
	if (model == NULL || !bar_keywheel_face_is_wheel(face))
		return 0;
	return model->spokes[face]->len;
}

/**
 * bar_keywheel_spoke:
 * @model: a model
 * @face: a wheel face
 * @index: a spoke
 *
 * Returns: (transfer none) (nullable): the spoke
 */
const BarKeywheelSpoke *
bar_keywheel_spoke(const BarKeywheelModel *model, BarKeywheelFace face,
                   guint index)
{
	if (index >= bar_keywheel_n_spokes(model, face))
		return NULL;
	return g_ptr_array_index(model->spokes[face], index);
}

/**
 * bar_keywheel_find_spoke:
 * @model: (nullable): a model
 * @face: a wheel face
 * @keysym: a normalised keysym
 *
 * Returns: the spoke on @face standing for @keysym, or -1
 */
gint
bar_keywheel_find_spoke(const BarKeywheelModel *model, BarKeywheelFace face,
                        guint keysym)
{
	guint i, j, n;

	n = bar_keywheel_n_spokes(model, face);
	for (i = 0; i < n; i++) {
		const BarKeywheelSpoke *s =
			g_ptr_array_index(model->spokes[face], i);

		for (j = 0; j < s->n_keysyms; j++)
			if (s->keysyms[j] == keysym)
				return (gint)i;
	}
	return -1;
}

/**
 * bar_keywheel_cell:
 * @model: (nullable): a model
 * @face: a wheel face
 * @spoke: a spoke
 * @ring: a ring
 * @alt: the Alt flip
 * @prefer: for a spoke standing for several keys, the one to look up
 *   first; 0 for none
 *
 * The bind in one cell.  When several binds share a chord the first
 * wins, which is also the one the compositor's dispatch runs.
 *
 * Returns: (transfer none) (nullable): the bind, or %NULL when the
 *   cell is free
 */
const BarKeywheelBind *
bar_keywheel_cell(const BarKeywheelModel *model, BarKeywheelFace face,
                  guint spoke, BarKeywheelRing ring, gboolean alt,
                  guint prefer)
{
	const BarKeywheelSpoke *s;
	guint want, pass, i, j;

	s = bar_keywheel_spoke(model, face, spoke);
	if (s == NULL)
		return NULL;
	want = bar_keywheel_ring_mods(ring, alt);

	/* Two passes: the preferred key first, then any key of the spoke
	   in the spoke's order, so the 1-9 cell answers for 1. */
	for (pass = 0; pass < 2; pass++) {
		for (j = 0; j < s->n_keysyms; j++) {
			if (pass == 0 && s->keysyms[j] != prefer)
				continue;
			for (i = 0; i < model->binds->len; i++) {
				const BarKeywheelBind *b =
					g_ptr_array_index(model->binds, i);

				if ((b->modifiers & KW_MOD_MASK) != want)
					continue;
				if (bar_keywheel_normalize_keysym(b->keysym,
				        b->modifiers) == s->keysyms[j])
					return b;
			}
		}
	}
	return NULL;
}

/**
 * bar_keywheel_bind:
 * @model: (nullable): a model
 * @index: a bind index
 *
 * Returns: (transfer none) (nullable): the bind
 */
const BarKeywheelBind *
bar_keywheel_bind(const BarKeywheelModel *model, guint index)
{
	if (model == NULL || index >= model->binds->len)
		return NULL;
	return g_ptr_array_index(model->binds, index);
}

/**
 * bar_keywheel_list_len:
 * @model: (nullable): a model
 * @face: a list face
 *
 * Returns: the rows on @face
 */
guint
bar_keywheel_list_len(const BarKeywheelModel *model, BarKeywheelFace face)
{
	if (model == NULL)
		return 0;
	if (face == BAR_KEYWHEEL_FACE_BEYOND)
		return model->beyond->len;
	if (face == BAR_KEYWHEEL_FACE_EMACS && model->emacs != NULL)
		return model->emacs->len;
	return 0;
}

/* ----------------------------------------------------------------
 * The cursor
 * ---------------------------------------------------------------- */

/**
 * bar_keywheel_cursor_reset:
 * @cursor: a cursor
 *
 * Core face, outer ring, nothing selected: how the wheel opens.
 */
void
bar_keywheel_cursor_reset(BarKeywheelCursor *cursor)
{
	g_return_if_fail(cursor != NULL);

	cursor->face   = BAR_KEYWHEEL_FACE_CORE;
	cursor->spoke  = -1;
	cursor->ring   = BAR_KEYWHEEL_RING_SUPER;
	cursor->alt    = FALSE;
	cursor->keysym = 0;
	cursor->row    = -1;
}

/**
 * bar_keywheel_cursor_face:
 * @model: (nullable): a model
 * @cursor: a cursor
 * @step: +1 for the next face, -1 for the previous; 0 only clamps
 *
 * Turns the wheel to another face, wrapping, and clears the selection
 * (a spoke number means nothing on a different face).
 */
void
bar_keywheel_cursor_face(const BarKeywheelModel *model,
                         BarKeywheelCursor *cursor, gint step)
{
	gint n, f;

	g_return_if_fail(cursor != NULL);

	n = (gint)bar_keywheel_face_count(model);
	f = ((gint)cursor->face + step) % n;
	if (f < 0)
		f += n;
	if (f != (gint)cursor->face) {
		cursor->spoke  = -1;
		cursor->row    = -1;
		cursor->keysym = 0;
	}
	cursor->face = (BarKeywheelFace)f;
}

static gboolean
kw_is_modifier_key(guint keysym, guint *which)
{
	switch (keysym) {
	case XKB_KEY_Shift_L:
	case XKB_KEY_Shift_R:
		*which = BAR_KEYWHEEL_MOD_SHIFT;
		return TRUE;
	case XKB_KEY_Control_L:
	case XKB_KEY_Control_R:
		*which = BAR_KEYWHEEL_MOD_CTRL;
		return TRUE;
	case XKB_KEY_Alt_L:
	case XKB_KEY_Alt_R:
	case XKB_KEY_Meta_L:
	case XKB_KEY_Meta_R:
		*which = BAR_KEYWHEEL_MOD_ALT;
		return TRUE;
	case XKB_KEY_Super_L:
	case XKB_KEY_Super_R:
	case XKB_KEY_Hyper_L:
	case XKB_KEY_Hyper_R:
		*which = BAR_KEYWHEEL_MOD_LOGO;
		return TRUE;
	case XKB_KEY_ISO_Level3_Shift:
	case XKB_KEY_ISO_Level5_Shift:
	case XKB_KEY_Caps_Lock:
	case XKB_KEY_Num_Lock:
	case XKB_KEY_Mode_switch:
		*which = 0;
		return TRUE;
	default:
		return FALSE;
	}
}

/* Selects @spoke on @face with the ring and flip @modifiers spell. */
static void
kw_land(BarKeywheelCursor *cursor, BarKeywheelFace face, gint spoke,
        guint keysym, guint modifiers)
{
	gboolean shift = (modifiers & BAR_KEYWHEEL_MOD_SHIFT) != 0;
	gboolean ctrl  = (modifiers & BAR_KEYWHEEL_MOD_CTRL) != 0;

	cursor->face   = face;
	cursor->spoke  = spoke;
	cursor->keysym = keysym;
	cursor->row    = -1;
	cursor->alt    = (modifiers & BAR_KEYWHEEL_MOD_ALT) != 0;
	if (shift && ctrl)
		cursor->ring = BAR_KEYWHEEL_RING_HUB;
	else if (shift)
		cursor->ring = BAR_KEYWHEEL_RING_SHIFT;
	else if (ctrl)
		cursor->ring = BAR_KEYWHEEL_RING_CTRL;
	else
		cursor->ring = BAR_KEYWHEEL_RING_SUPER;
}

/**
 * bar_keywheel_handle_key:
 * @model: (nullable): a model
 * @cursor: the cursor to move
 * @keysym: the key pressed, as the panel received it
 * @modifiers: the modifiers held
 *
 * What one key press in the panel means.  The host keeps Escape,
 * Return, space, Tab, j, k, Up and Down for its own navigation, so
 * none of those ever arrive here; the rest of the keyboard is the
 * wheel's:
 *
 * - A key (with or without Shift, Ctrl, Alt) jumps to its spoke and
 *   lights the ring the held modifiers name: Shift+q lands on q in the
 *   Super+Shift ring.  The current face is tried first, then Core,
 *   Letters, Keys; a key with no spoke but a non-Super bind selects
 *   that row of the Beyond Super list.
 * - A modifier pressed on its own previews its ring (Shift, Ctrl, both
 *   for the hub) or flips the Alt rings.  It must be claimed either
 *   way: an unclaimed key closes the panel, and every chord starts with
 *   a modifier press.
 * - Left and Right walk around the ring; Home and End step out to the
 *   rim and in to the hub; Page Up and Page Down turn the face.
 * - `/' opens the search field.
 * - Super chords are NOT claimed.  They belong to the window manager,
 *   and they are how somebody gets out of an overlay they did not mean
 *   to open.
 *
 * Returns: what the panel should do
 */
BarKeywheelKeyResult
bar_keywheel_handle_key(const BarKeywheelModel *model,
                        BarKeywheelCursor *cursor, guint keysym,
                        guint modifiers)
{
	static const BarKeywheelFace order[] = {
		BAR_KEYWHEEL_FACE_CORE, BAR_KEYWHEEL_FACE_LETTERS,
		BAR_KEYWHEEL_FACE_KEYS
	};
	guint which = 0, key, i, n;
	gint spoke;

	g_return_val_if_fail(cursor != NULL, BAR_KEYWHEEL_KEY_IGNORED);

	/* A modifier on its own: preview its ring. */
	if (kw_is_modifier_key(keysym, &which)) {
		switch (which) {
		case BAR_KEYWHEEL_MOD_SHIFT:
			cursor->ring = (cursor->ring == BAR_KEYWHEEL_RING_CTRL ||
			                cursor->ring == BAR_KEYWHEEL_RING_HUB)
				? BAR_KEYWHEEL_RING_HUB : BAR_KEYWHEEL_RING_SHIFT;
			return BAR_KEYWHEEL_KEY_MOVED;
		case BAR_KEYWHEEL_MOD_CTRL:
			cursor->ring = (cursor->ring == BAR_KEYWHEEL_RING_SHIFT ||
			                cursor->ring == BAR_KEYWHEEL_RING_HUB)
				? BAR_KEYWHEEL_RING_HUB : BAR_KEYWHEEL_RING_CTRL;
			return BAR_KEYWHEEL_KEY_MOVED;
		case BAR_KEYWHEEL_MOD_ALT:
			cursor->alt = !cursor->alt;
			return BAR_KEYWHEEL_KEY_MOVED;
		default:
			return BAR_KEYWHEEL_KEY_CLAIMED;
		}
	}

	if (modifiers & BAR_KEYWHEEL_MOD_LOGO)
		return BAR_KEYWHEEL_KEY_IGNORED;

	switch (keysym) {
	case XKB_KEY_Prior:
	case XKB_KEY_KP_Prior:
		bar_keywheel_cursor_face(model, cursor, -1);
		return BAR_KEYWHEEL_KEY_MOVED;
	case XKB_KEY_Next:
	case XKB_KEY_KP_Next:
		bar_keywheel_cursor_face(model, cursor, 1);
		return BAR_KEYWHEEL_KEY_MOVED;
	case XKB_KEY_Left:
	case XKB_KEY_Right:
		n = bar_keywheel_n_spokes(model, cursor->face);
		if (n == 0) {
			/* A list face has no ring to walk: turn instead. */
			bar_keywheel_cursor_face(model, cursor,
				keysym == XKB_KEY_Left ? -1 : 1);
			return BAR_KEYWHEEL_KEY_MOVED;
		}
		if (cursor->spoke < 0)
			cursor->spoke = 0;
		else if (keysym == XKB_KEY_Left)
			cursor->spoke = (cursor->spoke + (gint)n - 1) % (gint)n;
		else
			cursor->spoke = (cursor->spoke + 1) % (gint)n;
		cursor->keysym = 0;
		return BAR_KEYWHEEL_KEY_MOVED;
	case XKB_KEY_Home:
		if (cursor->ring > BAR_KEYWHEEL_RING_SUPER)
			cursor->ring = (BarKeywheelRing)(cursor->ring - 1);
		return BAR_KEYWHEEL_KEY_MOVED;
	case XKB_KEY_End:
		if (cursor->ring < BAR_KEYWHEEL_RING_HUB)
			cursor->ring = (BarKeywheelRing)(cursor->ring + 1);
		return BAR_KEYWHEEL_KEY_MOVED;
	case XKB_KEY_slash:
		if ((modifiers & (BAR_KEYWHEEL_MOD_SHIFT |
		                  BAR_KEYWHEEL_MOD_CTRL |
		                  BAR_KEYWHEEL_MOD_ALT)) == 0)
			return BAR_KEYWHEEL_KEY_SEARCH;
		break;
	default:
		break;
	}

	/* A key: find its spoke, current face first. */
	key = bar_keywheel_normalize_keysym(keysym, modifiers);
	if (bar_keywheel_face_is_wheel(cursor->face)) {
		spoke = bar_keywheel_find_spoke(model, cursor->face, key);
		if (spoke >= 0) {
			kw_land(cursor, cursor->face, spoke, key, modifiers);
			return BAR_KEYWHEEL_KEY_MOVED;
		}
	}
	for (i = 0; i < G_N_ELEMENTS(order); i++) {
		spoke = bar_keywheel_find_spoke(model, order[i], key);
		if (spoke >= 0) {
			kw_land(cursor, order[i], spoke, key, modifiers);
			return BAR_KEYWHEEL_KEY_MOVED;
		}
	}

	/* Not on the wheel: a bind without Super, maybe. */
	if (model != NULL) {
		for (i = 0; i < model->beyond->len; i++) {
			const BarKeywheelBind *b = g_ptr_array_index(model->binds,
				g_array_index(model->beyond, guint, i));

			if (bar_keywheel_normalize_keysym(b->keysym,
			        b->modifiers) == key) {
				cursor->face   = BAR_KEYWHEEL_FACE_BEYOND;
				cursor->spoke  = -1;
				cursor->row    = (gint)i;
				cursor->keysym = key;
				return BAR_KEYWHEEL_KEY_MOVED;
			}
		}
	}

	/* Unknown, but still ours: a stray key must not close the wheel. */
	return BAR_KEYWHEEL_KEY_CLAIMED;
}

/**
 * bar_keywheel_select_cell:
 * @model: (nullable): a model
 * @cursor: the cursor
 * @spoke: the clicked spoke
 * @ring: the clicked ring
 *
 * A click on a cell: selects it, or reports that it was already the
 * selection -- a second click runs it, since a panel has no
 * double-click.
 *
 * Returns: %TRUE when the cell was already selected
 */
gboolean
bar_keywheel_select_cell(const BarKeywheelModel *model,
                         BarKeywheelCursor *cursor, guint spoke,
                         BarKeywheelRing ring)
{
	const BarKeywheelSpoke *s;

	g_return_val_if_fail(cursor != NULL, FALSE);

	if (cursor->spoke == (gint)spoke && cursor->ring == ring)
		return TRUE;
	s = bar_keywheel_spoke(model, cursor->face, spoke);
	cursor->spoke  = (gint)spoke;
	cursor->ring   = ring;
	cursor->row    = -1;
	cursor->keysym = (s != NULL && s->n_keysyms > 0) ? s->keysyms[0] : 0;
	return FALSE;
}

/**
 * bar_keywheel_run_target:
 * @model: (nullable): a model
 * @cursor: the cursor
 * @modifiers: (out): the chord's modifiers, exactly as configured
 * @keysym: (out): the chord's keysym, exactly as configured
 * @flags: (out) (optional): the bind's flags
 *
 * What Enter runs.  The values are the bind's own, not the cursor's,
 * because the compositor matches its configured keysym exactly -- a
 * config saying `Super+Shift+Q' stores `Q', which the normalised spoke
 * key `q' would not match.
 *
 * Returns: %TRUE when there is a bind to run
 */
gboolean
bar_keywheel_run_target(const BarKeywheelModel *model,
                        const BarKeywheelCursor *cursor, guint *modifiers,
                        guint *keysym, guint *flags)
{
	const BarKeywheelBind *b = NULL;

	g_return_val_if_fail(cursor != NULL, FALSE);

	if (bar_keywheel_face_is_wheel(cursor->face)) {
		if (cursor->spoke < 0)
			return FALSE;
		b = bar_keywheel_cell(model, cursor->face, (guint)cursor->spoke,
		                      cursor->ring, cursor->alt, cursor->keysym);
	} else if (cursor->face == BAR_KEYWHEEL_FACE_BEYOND && model != NULL &&
	           cursor->row >= 0 &&
	           (guint)cursor->row < model->beyond->len) {
		b = bar_keywheel_bind(model, g_array_index(model->beyond, guint,
		                                           cursor->row));
	}
	if (b == NULL)
		return FALSE;

	*modifiers = b->modifiers;
	*keysym    = b->keysym;
	if (flags != NULL)
		*flags = b->flags;
	return TRUE;
}

/* ----------------------------------------------------------------
 * Search
 * ---------------------------------------------------------------- */

/* Does every term of @terms occur in one of @fields? */
static gboolean
kw_matches(gchar **terms, const gchar * const *fields, guint n_fields)
{
	g_autoptr(GString) hay = NULL;
	g_autofree gchar *folded = NULL;
	guint i;

	hay = g_string_new(NULL);
	for (i = 0; i < n_fields; i++) {
		if (fields[i] == NULL)
			continue;
		g_string_append(hay, fields[i]);
		g_string_append_c(hay, '\n');
	}
	folded = g_utf8_casefold(hay->str, -1);
	for (i = 0; terms[i] != NULL; i++)
		if (terms[i][0] != '\0' && strstr(folded, terms[i]) == NULL)
			return FALSE;
	return TRUE;
}

/**
 * bar_keywheel_search:
 * @model: (nullable): a model
 * @query: (nullable): what was typed
 * @max: the most results to collect
 * @binds_out: (element-type guint): receives matching bind indices
 * @emacs_out: (element-type guint) (nullable): receives matching Emacs
 *   entry indices
 *
 * Every whitespace-separated term must appear (case-insensitively) in
 * the description, the action, its argument or the chord.  Binds come
 * first, in config order; Emacs entries after them.  At most @max are
 * collected, but every match is counted, so the panel can say how many
 * it is not showing.
 *
 * Returns: the number of matches, collected or not
 */
guint
bar_keywheel_search(const BarKeywheelModel *model, const gchar *query,
                    guint max, GArray *binds_out, GArray *emacs_out)
{
	g_autofree gchar *valid = NULL;
	g_autofree gchar *folded = NULL;
	g_auto(GStrv) terms = NULL;
	guint i, total = 0;

	g_return_val_if_fail(binds_out != NULL, 0);

	if (model == NULL || query == NULL)
		return 0;
	/* Casefolding assumes UTF-8; a pasted query may not be. */
	valid  = g_utf8_make_valid(query, -1);
	folded = g_utf8_casefold(valid, -1);
	g_strstrip(folded);
	if (folded[0] == '\0')
		return 0;
	terms = g_strsplit_set(folded, " \t", -1);

	for (i = 0; i < model->binds->len; i++) {
		const BarKeywheelBind *b = g_ptr_array_index(model->binds, i);
		const gchar *fields[4];

		fields[0] = b->desc;
		fields[1] = b->action;
		fields[2] = b->arg;
		fields[3] = b->combo;
		if (!kw_matches(terms, fields, G_N_ELEMENTS(fields)))
			continue;
		if (total < max)
			g_array_append_val(binds_out, i);
		total++;
	}
	if (model->emacs != NULL) {
		for (i = 0; i < model->emacs->len; i++) {
			const BarKeywheelEmacs *e =
				g_ptr_array_index(model->emacs, i);
			const gchar *fields[3];

			fields[0] = e->key;
			fields[1] = e->command;
			fields[2] = e->doc;
			if (!kw_matches(terms, fields, G_N_ELEMENTS(fields)))
				continue;
			if (total < max && emacs_out != NULL)
				g_array_append_val(emacs_out, i);
			total++;
		}
	}
	return total;
}

/* ----------------------------------------------------------------
 * The Emacs face
 * ---------------------------------------------------------------- */

/**
 * bar_keywheel_unescape:
 * @text: one escaped field
 *
 * Undoes the cmacs side's escaping: `\\' is a backslash, `\t' a tab
 * and `\n' a newline.  Fields are joined by real tabs, so a tab inside
 * one has to be escaped; an unknown escape keeps its character.
 *
 * Returns: (transfer full): the field
 */
gchar *
bar_keywheel_unescape(const gchar *text)
{
	GString *out;
	const gchar *p;

	out = g_string_new(NULL);
	for (p = text; p != NULL && *p != '\0'; p++) {
		if (*p != '\\' || p[1] == '\0') {
			g_string_append_c(out, *p);
			continue;
		}
		p++;
		switch (*p) {
		case 't':
			g_string_append_c(out, '\t');
			break;
		case 'n':
			g_string_append_c(out, '\n');
			break;
		default:
			g_string_append_c(out, *p);
			break;
		}
	}
	return g_string_free(out, FALSE);
}

/* An unescaped field, made safe to draw on one line. */
static gchar *
kw_field(const gchar *raw)
{
	g_autofree gchar *plain = NULL;
	gchar *clean;

	plain = bar_keywheel_unescape(raw);
	clean = kw_clean(plain);
	if (clean != NULL)
		g_strdelimit(clean, "\t\n\r", ' ');
	return clean;
}

/**
 * bar_keywheel_emacs_parse:
 * @line: (nullable): one pushed `emacs.<n>' value, "key\tcommand\tdoc"
 *
 * Returns: (transfer full) (nullable): the entry, or %NULL when the
 *   line has no key or no command
 */
BarKeywheelEmacs *
bar_keywheel_emacs_parse(const gchar *line)
{
	g_auto(GStrv) parts = NULL;
	BarKeywheelEmacs *e;

	if (line == NULL || line[0] == '\0')
		return NULL;
	parts = g_strsplit(line, "\t", 3);
	if (parts[0] == NULL || parts[1] == NULL)
		return NULL;

	e = g_new0(BarKeywheelEmacs, 1);
	e->key     = kw_field(parts[0]);
	e->command = kw_field(parts[1]);
	e->doc     = (parts[2] != NULL) ? kw_field(parts[2]) : NULL;
	if (e->key == NULL || e->command == NULL) {
		bar_keywheel_emacs_free(e);
		return NULL;
	}
	return e;
}

/**
 * bar_keywheel_emacs_free:
 * @entry: (nullable) (transfer full): an entry
 */
void
bar_keywheel_emacs_free(BarKeywheelEmacs *entry)
{
	if (entry == NULL)
		return;
	g_free(entry->key);
	g_free(entry->command);
	g_free(entry->doc);
	g_free(entry);
}

/* ----------------------------------------------------------------
 * Settings
 * ---------------------------------------------------------------- */

/**
 * bar_keywheel_parse_size:
 * @spec: (nullable): `WIDTHxHEIGHT', e.g. "640x560"
 * @width: (out): the canvas width
 * @height: (out): the canvas height
 *
 * Anything unparseable is the default; anything parseable is clamped
 * to a size the wheel can still be drawn and clicked at.
 */
void
bar_keywheel_parse_size(const gchar *spec, gint *width, gint *height)
{
	gint64 w = 640, h = 560;
	gchar *end = NULL;

	if (spec != NULL && spec[0] != '\0') {
		gint64 pw, ph;

		pw = g_ascii_strtoll(spec, &end, 10);
		if (end != NULL && end != spec && (*end == 'x' || *end == 'X')) {
			const gchar *rest = end + 1;

			ph = g_ascii_strtoll(rest, &end, 10);
			if (end != NULL && end != rest && *end == '\0') {
				w = pw;
				h = ph;
			}
		}
	}
	*width  = (gint)CLAMP(w, 360, 1600);
	*height = (gint)CLAMP(h, 320, 1400);
}

/* The palette roles, in GowlBarColor order.  A copy rather than a call
   into the theme so this file stays free of it; the tests check every
   name against gowl_bar_theme_color_name(). */
static const gchar * const kw_roles[] = {
	"base", "mantle", "crust", "surface", "surface-alt", "overlay",
	"text", "subtext", "muted", "accent", "red", "green", "yellow",
	"blue", "mauve", "teal", "peach", "pink", "sapphire", "sky",
	"lavender", "flamingo", "rosewater", "maroon"
};

/**
 * bar_keywheel_role_from_name:
 * @name: (nullable): a role name, any case
 * @role: (out): the role's GowlBarColor value
 *
 * Returns: %TRUE when @name is a role
 */
gboolean
bar_keywheel_role_from_name(const gchar *name, gint *role)
{
	guint i;

	if (name == NULL)
		return FALSE;
	for (i = 0; i < G_N_ELEMENTS(kw_roles); i++) {
		if (g_ascii_strcasecmp(name, kw_roles[i]) == 0) {
			*role = (gint)i;
			return TRUE;
		}
	}
	return FALSE;
}

/**
 * bar_keywheel_role_name:
 * @role: a GowlBarColor value
 *
 * Returns: (transfer none) (nullable): its name
 */
const gchar *
bar_keywheel_role_name(gint role)
{
	if (role < 0 || role >= (gint)G_N_ELEMENTS(kw_roles))
		return NULL;
	return kw_roles[role];
}

/* `#rrggbb' or `#rrggbbaa', every digit hexadecimal. */
static gboolean
kw_parse_hex(const gchar *spec, gdouble rgba[4])
{
	gsize len, i;
	guint v[4];

	if (spec == NULL || spec[0] != '#')
		return FALSE;
	len = strlen(spec);
	if (len != 7 && len != 9)
		return FALSE;
	for (i = 1; i < len; i++)
		if (!g_ascii_isxdigit(spec[i]))
			return FALSE;

	v[3] = 255;
	for (i = 0; i < (len - 1) / 2; i++)
		v[i] = (guint)(g_ascii_xdigit_value(spec[1 + 2 * i]) * 16 +
		               g_ascii_xdigit_value(spec[2 + 2 * i]));
	for (i = 0; i < 4; i++)
		rgba[i] = (gdouble)v[i] / 255.0;
	return TRUE;
}

/**
 * bar_keywheel_color_resolve:
 * @spec: (nullable): a `color-*' setting
 * @fallback: the role to use when @spec is unset or unusable
 * @role: (out): the role to paint with (meaningful unless HEX)
 * @rgba: (out caller-allocates) (array fixed-size=4): the literal, for
 *   HEX
 *
 * The bar's three-way colour rule, for a canvas: a role name follows
 * the session palette, a `#rrggbb' literal is exactly that colour, and
 * anything else -- including garbage -- is the default role rather
 * than black or nothing.
 *
 * Returns: which of the three @spec was
 */
BarKeywheelColorKind
bar_keywheel_color_resolve(const gchar *spec, gint fallback, gint *role,
                           gdouble rgba[4])
{
	gint r;

	*role = fallback;
	if (spec == NULL || spec[0] == '\0')
		return BAR_KEYWHEEL_COLOR_FALLBACK;
	if (bar_keywheel_role_from_name(spec, &r)) {
		*role = r;
		return BAR_KEYWHEEL_COLOR_ROLE;
	}
	if (kw_parse_hex(spec, rgba))
		return BAR_KEYWHEEL_COLOR_HEX;
	return BAR_KEYWHEEL_COLOR_FALLBACK;
}

/* ----------------------------------------------------------------
 * Geometry
 * ---------------------------------------------------------------- */

#define KW_PAD       (8.0)
#define KW_HEADER_H  (30.0)
#define KW_DOTS_H    (22.0)
#define KW_DETAILS_H (88.0)

/* Ring bands as fractions of the radius, outermost first.  The small
   gaps between them are what keeps two rings from both claiming a
   point on their shared edge. */
static const gdouble kw_ring_band[BAR_KEYWHEEL_RING_COUNT][2] = {
	{ 0.765, 1.000 },
	{ 0.540, 0.755 },
	{ 0.330, 0.530 },
	{ 0.150, 0.320 }
};

/**
 * bar_keywheel_geom:
 * @width: the canvas width
 * @height: the canvas height
 * @n_spokes: spokes on the face (ignored for a list face)
 * @n_faces: how many faces there are, for the dots
 * @wheel: whether the face is drawn as a wheel
 * @geom: (out caller-allocates): the layout
 *
 * Top to bottom: a header (face title, Alt switch), the wheel, the
 * face dots, and a details box for the selection.  A list face keeps
 * only the header and the dots; its rows are ordinary panel rows
 * under the canvas.
 */
void
bar_keywheel_geom(gdouble width, gdouble height, guint n_spokes,
                  guint n_faces, gboolean wheel, BarKeywheelGeom *geom)
{
	gdouble top, bottom, r;
	guint i;

	memset(geom, 0, sizeof(*geom));
	geom->width    = width;
	geom->height   = height;
	geom->wheel    = wheel && n_spokes > 0;
	geom->n_spokes = n_spokes;
	geom->n_faces  = (n_faces > 0) ? n_faces : 1;
	geom->header_h = KW_HEADER_H;

	/* The Alt switch, at the header's right. */
	geom->alt_w = 52.0;
	geom->alt_h = 20.0;
	geom->alt_x = width - KW_PAD - geom->alt_w;
	geom->alt_y = (KW_HEADER_H - geom->alt_h) / 2.0;

	geom->dot_r    = 4.0;
	geom->dot_step = 18.0;

	if (!geom->wheel) {
		geom->dots_y = KW_HEADER_H + KW_DOTS_H / 2.0;
		geom->dot_x0 = width / 2.0 -
			geom->dot_step * (gdouble)(geom->n_faces - 1) / 2.0;
		return;
	}

	top    = KW_HEADER_H;
	bottom = height - KW_DETAILS_H - KW_DOTS_H - KW_PAD;
	r = MIN(width / 2.0 - KW_PAD, (bottom - top) / 2.0 - 2.0);
	if (r < 20.0)
		r = 20.0;
	geom->radius = r;
	geom->cx = width / 2.0;
	geom->cy = top + (bottom - top) / 2.0;
	for (i = 0; i < BAR_KEYWHEEL_RING_COUNT; i++) {
		geom->ring_in[i]  = r * kw_ring_band[i][0];
		geom->ring_out[i] = r * kw_ring_band[i][1];
	}
	geom->hub_r = geom->ring_in[BAR_KEYWHEEL_RING_HUB] - 2.0;

	geom->dots_y = bottom + KW_DOTS_H / 2.0;
	geom->dot_x0 = width / 2.0 -
		geom->dot_step * (gdouble)(geom->n_faces - 1) / 2.0;

	geom->details_x = KW_PAD;
	geom->details_y = height - KW_DETAILS_H;
	geom->details_w = width - 2.0 * KW_PAD;
	geom->details_h = KW_DETAILS_H - KW_PAD / 2.0;
}

/**
 * bar_keywheel_spoke_span:
 * @geom: a layout
 * @spoke: a spoke
 * @a0: (out): its first angle, radians, screen orientation
 * @a1: (out): its last angle
 *
 * Spoke 0 is centred at the top and the rest follow clockwise.
 */
void
bar_keywheel_spoke_span(const BarKeywheelGeom *geom, guint spoke,
                        gdouble *a0, gdouble *a1)
{
	gdouble step, mid;

	step = 2.0 * G_PI / (gdouble)MAX(geom->n_spokes, 1u);
	mid  = -G_PI / 2.0 + step * (gdouble)spoke;
	*a0 = mid - step / 2.0;
	*a1 = mid + step / 2.0;
}

/**
 * bar_keywheel_cell_center:
 * @geom: a wheel layout
 * @spoke: a spoke
 * @ring: a ring
 * @x: (out): canvas x of the cell's centre
 * @y: (out): canvas y
 */
void
bar_keywheel_cell_center(const BarKeywheelGeom *geom, guint spoke,
                         BarKeywheelRing ring, gdouble *x, gdouble *y)
{
	gdouble a0, a1, mid, rad;

	bar_keywheel_spoke_span(geom, spoke, &a0, &a1);
	mid = (a0 + a1) / 2.0;
	rad = (geom->ring_in[ring] + geom->ring_out[ring]) / 2.0;
	*x = geom->cx + rad * cos(mid);
	*y = geom->cy + rad * sin(mid);
}

/**
 * bar_keywheel_dot_center:
 * @geom: a layout
 * @face: a face index
 * @x: (out): canvas x of its dot
 * @y: (out): canvas y
 */
void
bar_keywheel_dot_center(const BarKeywheelGeom *geom, guint face,
                        gdouble *x, gdouble *y)
{
	*x = geom->dot_x0 + geom->dot_step * (gdouble)face;
	*y = geom->dots_y;
}

/**
 * bar_keywheel_hit:
 * @geom: a layout
 * @x: canvas x
 * @y: canvas y
 * @spoke: (out): the spoke, for a cell
 * @ring: (out): the ring, for a cell
 * @face: (out): the face, for a dot
 *
 * What a click at (@x, @y) landed on.  Uses the same layout as the
 * painter, so every cell is hit exactly where it is drawn.
 *
 * Returns: the kind of thing hit
 */
BarKeywheelHit
bar_keywheel_hit(const BarKeywheelGeom *geom, gdouble x, gdouble y,
                 gint *spoke, BarKeywheelRing *ring, gint *face)
{
	gdouble dx, dy, dist, step, a;
	guint i;

	*spoke = -1;
	*face  = -1;
	*ring  = BAR_KEYWHEEL_RING_SUPER;

	/* The Alt pill in the header. */
	if (x >= geom->alt_x && x <= geom->alt_x + geom->alt_w &&
	    y >= geom->alt_y && y <= geom->alt_y + geom->alt_h)
		return BAR_KEYWHEEL_HIT_ALT;

	/* The face dots: a generous square around each, since a 4 px dot
	   is a hard target. */
	for (i = 0; i < geom->n_faces; i++) {
		gdouble fx, fy;

		bar_keywheel_dot_center(geom, i, &fx, &fy);
		if (fabs(x - fx) <= geom->dot_step / 2.0 &&
		    fabs(y - fy) <= geom->dot_step / 2.0) {
			*face = (gint)i;
			return BAR_KEYWHEEL_HIT_DOT;
		}
	}

	if (!geom->wheel)
		return BAR_KEYWHEEL_HIT_NONE;

	dx = x - geom->cx;
	dy = y - geom->cy;
	dist = sqrt(dx * dx + dy * dy);
	if (dist <= geom->hub_r)
		return BAR_KEYWHEEL_HIT_ALT;

	for (i = 0; i < BAR_KEYWHEEL_RING_COUNT; i++) {
		if (dist < geom->ring_in[i] || dist > geom->ring_out[i])
			continue;

		/* Angle from the top of spoke 0's span, clockwise. */
		step = 2.0 * G_PI / (gdouble)MAX(geom->n_spokes, 1u);
		a = atan2(dy, dx) + G_PI / 2.0 + step / 2.0;
		while (a < 0.0)
			a += 2.0 * G_PI;
		while (a >= 2.0 * G_PI)
			a -= 2.0 * G_PI;
		*spoke = (gint)floor(a / step);
		if (*spoke >= (gint)geom->n_spokes)
			*spoke = (gint)geom->n_spokes - 1;
		*ring = (BarKeywheelRing)i;
		return BAR_KEYWHEEL_HIT_CELL;
	}
	return BAR_KEYWHEEL_HIT_NONE;
}
