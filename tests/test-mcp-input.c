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
 * test-mcp-input.c - Unit tests for MCP input injection XKB logic.
 *
 * The core keyboard injection functions (keysym_to_keycode and
 * find_mod_key_evdev) are static inside gowl-mcp-tools-input.c and
 * cannot be called from external test binaries.  This file contains
 * inline re-implementations of the same algorithms and tests them
 * exhaustively against a real US QWERTY xkb keymap.
 *
 * Tests verify:
 *   - keysym_to_keycode: reverse keymap lookup for ASCII characters
 *   - find_mod_key_evdev: modifier-index → evdev-keycode mapping
 *   - xkb_utf32_to_keysym: Unicode → keysym for URL-special chars
 *   - modifier mask resolution for shifted characters (&, ?, :, etc.)
 *   - Full URL character coverage (no char is silently dropped)
 */

#include <glib.h>
#include <xkbcommon/xkbcommon.h>
#include <linux/input-event-codes.h>
#include <string.h>

/* Global keymap shared across all tests (US QWERTY, read-only) */
static struct xkb_context *g_ctx    = NULL;
static struct xkb_keymap  *g_keymap = NULL;

/* ========================================================================== */
/* Inline implementations of the functions under test                         */
/*                                                                            */
/* These are exact copies of the static functions in                          */
/* modules/mcp/gowl-mcp-tools-input.c.  Any divergence is a bug.            */
/* ========================================================================== */

/*
 * test_keysym_to_keycode:
 *
 * Reverse-lookup: finds the first keycode in the keymap that produces
 * @keysym at any layout/level.  Returns 0 if not found.
 */
static xkb_keycode_t
test_keysym_to_keycode(
	struct xkb_keymap  *keymap,
	xkb_keysym_t        keysym,
	xkb_layout_index_t *out_layout,
	xkb_level_index_t  *out_level
){
	xkb_keycode_t min_kc;
	xkb_keycode_t max_kc;
	xkb_keycode_t kc;

	min_kc = xkb_keymap_min_keycode(keymap);
	max_kc = xkb_keymap_max_keycode(keymap);

	for (kc = min_kc; kc <= max_kc; kc++) {
		xkb_layout_index_t num_layouts;
		xkb_layout_index_t layout;

		num_layouts = xkb_keymap_num_layouts_for_key(keymap, kc);
		for (layout = 0; layout < num_layouts; layout++) {
			xkb_level_index_t num_levels;
			xkb_level_index_t level;

			num_levels = xkb_keymap_num_levels_for_key(
				keymap, kc, layout);
			for (level = 0; level < num_levels; level++) {
				const xkb_keysym_t *syms;
				int nsyms;
				int i;

				nsyms = xkb_keymap_key_get_syms_by_level(
					keymap, kc, layout, level, &syms);
				for (i = 0; i < nsyms; i++) {
					if (syms[i] == keysym) {
						if (out_layout != NULL)
							*out_layout = layout;
						if (out_level != NULL)
							*out_level = level;
						return kc;
					}
				}
			}
		}
	}

	if (out_layout != NULL)
		*out_layout = 0;
	if (out_level != NULL)
		*out_level = 0;
	return 0;
}

/*
 * test_find_mod_key_evdev:
 *
 * Maps an xkb modifier index to the evdev keycode of a suitable physical
 * modifier key.  Returns 0 if not found.
 */
static guint32
test_find_mod_key_evdev(
	struct xkb_keymap *keymap,
	xkb_mod_index_t    mod_index
){
	static const struct {
		const gchar  *mod_name;
		xkb_keysym_t  keysym;
	} mod_table[] = {
		{ XKB_MOD_NAME_SHIFT, XKB_KEY_Shift_L         },
		{ XKB_MOD_NAME_CAPS,  XKB_KEY_Caps_Lock        },
		{ XKB_MOD_NAME_CTRL,  XKB_KEY_Control_L        },
		{ XKB_MOD_NAME_ALT,   XKB_KEY_Alt_L            },
		{ XKB_MOD_NAME_NUM,   XKB_KEY_Num_Lock         },
		{ XKB_MOD_NAME_LOGO,  XKB_KEY_Super_L          },
		{ "Mod3",             XKB_KEY_ISO_Level3_Shift },
		{ "Mod5",             XKB_KEY_ISO_Level5_Shift },
		{ NULL, 0 }
	};
	const gchar   *mod_name;
	xkb_keycode_t  kc;
	gint           i;

	mod_name = xkb_keymap_mod_get_name(keymap, mod_index);
	if (mod_name == NULL)
		return 0;

	for (i = 0; mod_table[i].mod_name != NULL; i++) {
		if (g_strcmp0(mod_name, mod_table[i].mod_name) == 0) {
			kc = test_keysym_to_keycode(
				keymap, mod_table[i].keysym, NULL, NULL);
			if (kc != 0)
				return (guint32)(kc - 8); /* xkb → evdev */
		}
	}
	return 0;
}

/* ========================================================================== */
/* Helpers                                                                    */
/* ========================================================================== */

/*
 * get_shift_mod_index:
 *
 * Returns the xkb modifier index for the Shift modifier in the keymap,
 * or XKB_MOD_INVALID if not found.
 */
static xkb_mod_index_t
get_shift_mod_index(struct xkb_keymap *keymap)
{
	return xkb_keymap_mod_get_index(keymap, XKB_MOD_NAME_SHIFT);
}

/*
 * char_level:
 *
 * Returns the keymap level at which @unicode_char lives, or -1 if the
 * character is not found in the keymap at all.
 */
static gint
char_level(struct xkb_keymap *keymap, gunichar unicode_char)
{
	xkb_keysym_t keysym;
	xkb_keycode_t kc;
	xkb_level_index_t level;

	keysym = xkb_utf32_to_keysym((guint32)unicode_char);
	if (keysym == XKB_KEY_NoSymbol)
		return -1;

	kc = test_keysym_to_keycode(keymap, keysym, NULL, &level);
	if (kc == 0)
		return -1;

	return (gint)level;
}

/*
 * level1_uses_shift:
 *
 * Returns TRUE if the level-1 entry for the key at @xkb_kc in layout 0
 * requires the Shift modifier (i.e. Shift's bit is in masks[0]).
 */
static gboolean
level1_uses_shift(struct xkb_keymap *keymap, xkb_keycode_t xkb_kc)
{
	xkb_mod_mask_t masks[8];
	size_t nmasks;
	xkb_mod_index_t shift_idx;
	xkb_mod_index_t bit;

	nmasks = xkb_keymap_key_get_mods_for_level(
		keymap, xkb_kc, 0, 1, masks, G_N_ELEMENTS(masks));
	if (nmasks == 0)
		return FALSE;

	shift_idx = get_shift_mod_index(keymap);
	if (shift_idx == XKB_MOD_INVALID)
		return FALSE;

	bit = shift_idx;
	return (masks[0] & (1u << bit)) != 0;
}

/* ========================================================================== */
/* Tests: keysym_to_keycode                                                   */
/* ========================================================================== */

static void
test_ktk_level0_lowercase(void)
{
	/* Lowercase ASCII letters are at level 0 on a US keyboard */
	static const xkb_keysym_t letters[] = {
		XKB_KEY_a, XKB_KEY_b, XKB_KEY_z, XKB_KEY_m
	};
	gsize i;

	for (i = 0; i < G_N_ELEMENTS(letters); i++) {
		xkb_layout_index_t layout;
		xkb_level_index_t level;
		xkb_keycode_t kc;

		kc = test_keysym_to_keycode(g_keymap, letters[i],
			&layout, &level);
		g_assert_cmpuint(kc, !=, 0);
		g_assert_cmpuint(level, ==, 0);
	}
}

static void
test_ktk_level1_uppercase(void)
{
	/* Uppercase ASCII letters are at level 1 on a US keyboard (Shift) */
	static const xkb_keysym_t letters[] = {
		XKB_KEY_A, XKB_KEY_B, XKB_KEY_Z, XKB_KEY_M
	};
	gsize i;

	for (i = 0; i < G_N_ELEMENTS(letters); i++) {
		xkb_layout_index_t layout;
		xkb_level_index_t level;
		xkb_keycode_t kc;

		kc = test_keysym_to_keycode(g_keymap, letters[i],
			&layout, &level);
		g_assert_cmpuint(kc, !=, 0);
		g_assert_cmpuint(level, ==, 1);
	}
}

static void
test_ktk_url_unshifted(void)
{
	/*
	 * These URL characters require no modifier on a US keyboard:
	 *   / = - . digits
	 */
	static const xkb_keysym_t syms[] = {
		XKB_KEY_slash,
		XKB_KEY_equal,
		XKB_KEY_minus,
		XKB_KEY_period,
		XKB_KEY_0, XKB_KEY_1, XKB_KEY_2, XKB_KEY_3,
		XKB_KEY_4, XKB_KEY_5, XKB_KEY_6, XKB_KEY_7,
		XKB_KEY_8, XKB_KEY_9,
	};
	gsize i;

	for (i = 0; i < G_N_ELEMENTS(syms); i++) {
		xkb_level_index_t level;
		xkb_keycode_t kc;

		kc = test_keysym_to_keycode(g_keymap, syms[i], NULL, &level);
		g_assert_cmpuint(kc, !=, 0);
		g_assert_cmpuint(level, ==, 0);
	}
}

static void
test_ktk_url_shifted(void)
{
	/*
	 * These URL characters live at level 1 (Shift required) on a US keyboard:
	 *   & ? : @ ! # $ % ^ * ( ) _ + < > | ~ "
	 *
	 * The bug this test guards against: without real Shift key events,
	 * Firefox receives the unshifted keysym (7, /, ;, etc.) instead of
	 * the shifted one (&, ?, :, etc.).
	 */
	static const xkb_keysym_t syms[] = {
		XKB_KEY_ampersand,   /* Shift+7  — the original bug trigger  */
		XKB_KEY_question,    /* Shift+/  — common in URL query params */
		XKB_KEY_colon,       /* Shift+;  — appears in http://         */
		XKB_KEY_at,          /* Shift+2  — user@host                  */
		XKB_KEY_exclam,      /* Shift+1                               */
		XKB_KEY_numbersign,  /* Shift+3  — URL fragment               */
		XKB_KEY_dollar,      /* Shift+4                               */
		XKB_KEY_percent,     /* Shift+5  — percent-encoding prefix    */
		XKB_KEY_asciicircum, /* Shift+6                               */
		XKB_KEY_asterisk,    /* Shift+8                               */
		XKB_KEY_parenleft,   /* Shift+9                               */
		XKB_KEY_parenright,  /* Shift+0                               */
		XKB_KEY_underscore,  /* Shift+-                               */
		XKB_KEY_plus,        /* Shift+=  — encoded space in URLs      */
		XKB_KEY_less,        /* Shift+,                               */
		XKB_KEY_greater,     /* Shift+.                               */
		XKB_KEY_bar,         /* Shift+\                               */
		XKB_KEY_asciitilde,  /* Shift+`                               */
		XKB_KEY_quotedbl,    /* Shift+'                               */
	};
	gsize i;

	for (i = 0; i < G_N_ELEMENTS(syms); i++) {
		xkb_level_index_t level;
		xkb_keycode_t kc;

		kc = test_keysym_to_keycode(g_keymap, syms[i], NULL, &level);
		g_assert_cmpuint(kc, !=, 0);
		g_assert_cmpuint(level, ==, 1);
	}
}

static void
test_ktk_modifier_keys_findable(void)
{
	/*
	 * The modifier keys themselves must be findable in the keymap
	 * so that find_mod_key_evdev can return their evdev codes.
	 */
	static const xkb_keysym_t mods[] = {
		XKB_KEY_Shift_L,
		XKB_KEY_Shift_R,
		XKB_KEY_Control_L,
		XKB_KEY_Control_R,
		XKB_KEY_Alt_L,
	};
	gsize i;

	for (i = 0; i < G_N_ELEMENTS(mods); i++) {
		xkb_keycode_t kc;

		kc = test_keysym_to_keycode(g_keymap, mods[i], NULL, NULL);
		g_assert_cmpuint(kc, !=, 0);
	}
}

static void
test_ktk_not_found_returns_zero(void)
{
	/*
	 * Keysyms that have no physical key in the keymap must return 0.
	 * XKB_KEY_NoSymbol is the canonical "no match" sentinel.
	 */
	xkb_keycode_t kc;

	kc = test_keysym_to_keycode(g_keymap, XKB_KEY_NoSymbol, NULL, NULL);
	g_assert_cmpuint(kc, ==, 0);
}

static void
test_ktk_idempotent(void)
{
	/* Same keysym must return the same keycode every time */
	xkb_keycode_t kc1;
	xkb_keycode_t kc2;
	xkb_level_index_t lv1;
	xkb_level_index_t lv2;

	kc1 = test_keysym_to_keycode(g_keymap, XKB_KEY_ampersand, NULL, &lv1);
	kc2 = test_keysym_to_keycode(g_keymap, XKB_KEY_ampersand, NULL, &lv2);
	g_assert_cmpuint(kc1, ==, kc2);
	g_assert_cmpuint(lv1, ==, lv2);
}

static void
test_ktk_shift_l_evdev_correct(void)
{
	/*
	 * Shift_L must be at xkb keycode 50 (evdev 42 = KEY_LEFTSHIFT).
	 * This is the concrete value find_mod_key_evdev must return when
	 * the caller requests the Shift modifier key code.
	 */
	xkb_keycode_t kc;
	guint32 evdev;

	kc = test_keysym_to_keycode(g_keymap, XKB_KEY_Shift_L, NULL, NULL);
	g_assert_cmpuint(kc, !=, 0);

	/* xkb keycode = evdev + 8, so evdev = kc - 8 */
	evdev = (guint32)(kc - 8);
	g_assert_cmpuint(evdev, ==, (guint32)KEY_LEFTSHIFT);
}

/* ========================================================================== */
/* Tests: find_mod_key_evdev                                                  */
/* ========================================================================== */

static void
test_fmke_shift_returns_leftshift(void)
{
	/*
	 * The Shift modifier index must yield KEY_LEFTSHIFT (42).
	 * This is the evdev code that send_text emits for e.g. '&'.
	 */
	xkb_mod_index_t shift_idx;
	guint32 evdev;

	shift_idx = xkb_keymap_mod_get_index(g_keymap, XKB_MOD_NAME_SHIFT);
	g_assert_cmpuint(shift_idx, !=, XKB_MOD_INVALID);

	evdev = test_find_mod_key_evdev(g_keymap, shift_idx);
	g_assert_cmpuint(evdev, ==, (guint32)KEY_LEFTSHIFT);
}

static void
test_fmke_ctrl_returns_nonzero(void)
{
	xkb_mod_index_t ctrl_idx;
	guint32 evdev;

	ctrl_idx = xkb_keymap_mod_get_index(g_keymap, XKB_MOD_NAME_CTRL);
	g_assert_cmpuint(ctrl_idx, !=, XKB_MOD_INVALID);

	evdev = test_find_mod_key_evdev(g_keymap, ctrl_idx);
	g_assert_cmpuint(evdev, !=, 0);
}

static void
test_fmke_alt_returns_nonzero(void)
{
	xkb_mod_index_t alt_idx;
	guint32 evdev;

	alt_idx = xkb_keymap_mod_get_index(g_keymap, XKB_MOD_NAME_ALT);
	g_assert_cmpuint(alt_idx, !=, XKB_MOD_INVALID);

	evdev = test_find_mod_key_evdev(g_keymap, alt_idx);
	g_assert_cmpuint(evdev, !=, 0);
}

static void
test_fmke_invalid_index_returns_zero(void)
{
	/*
	 * XKB_MOD_INVALID must return 0 (xkb_keymap_mod_get_name returns
	 * NULL for an out-of-range index, so the function short-circuits).
	 */
	guint32 evdev;

	evdev = test_find_mod_key_evdev(g_keymap, XKB_MOD_INVALID);
	g_assert_cmpuint(evdev, ==, 0);
}

static void
test_fmke_known_mods_sane_evdev(void)
{
	/*
	 * All modifier indices that are present in the keymap must return
	 * either 0 (no key found) or a plausible evdev code (< 256).
	 * This guards against keycodes being corrupted by the xkb-8 offset.
	 */
	xkb_mod_index_t num_mods;
	xkb_mod_index_t i;

	num_mods = xkb_keymap_num_mods(g_keymap);
	for (i = 0; i < num_mods; i++) {
		guint32 evdev;

		evdev = test_find_mod_key_evdev(g_keymap, i);
		/* Either not found (0) or a sane evdev code */
		g_assert_true(evdev == 0 || evdev < 256);
	}
}

/* ========================================================================== */
/* Tests: xkb_utf32_to_keysym for URL-special characters                     */
/* ========================================================================== */

static void
test_utf32_ampersand(void)
{
	xkb_keysym_t ks;

	ks = xkb_utf32_to_keysym(0x0026u); /* U+0026 = '&' */
	g_assert_cmpuint(ks, ==, XKB_KEY_ampersand);
}

static void
test_utf32_question(void)
{
	xkb_keysym_t ks;

	ks = xkb_utf32_to_keysym(0x003Fu); /* U+003F = '?' */
	g_assert_cmpuint(ks, ==, XKB_KEY_question);
}

static void
test_utf32_colon(void)
{
	xkb_keysym_t ks;

	ks = xkb_utf32_to_keysym(0x003Au); /* U+003A = ':' */
	g_assert_cmpuint(ks, ==, XKB_KEY_colon);
}

static void
test_utf32_slash(void)
{
	xkb_keysym_t ks;

	ks = xkb_utf32_to_keysym(0x002Fu); /* U+002F = '/' */
	g_assert_cmpuint(ks, ==, XKB_KEY_slash);
}

static void
test_utf32_at(void)
{
	xkb_keysym_t ks;

	ks = xkb_utf32_to_keysym(0x0040u); /* U+0040 = '@' */
	g_assert_cmpuint(ks, ==, XKB_KEY_at);
}

static void
test_utf32_equals(void)
{
	xkb_keysym_t ks;

	ks = xkb_utf32_to_keysym(0x003Du); /* U+003D = '=' */
	g_assert_cmpuint(ks, ==, XKB_KEY_equal);
}

static void
test_utf32_hash(void)
{
	xkb_keysym_t ks;

	ks = xkb_utf32_to_keysym(0x0023u); /* U+0023 = '#' */
	g_assert_cmpuint(ks, ==, XKB_KEY_numbersign);
}

static void
test_utf32_percent(void)
{
	xkb_keysym_t ks;

	ks = xkb_utf32_to_keysym(0x0025u); /* U+0025 = '%' */
	g_assert_cmpuint(ks, ==, XKB_KEY_percent);
}

/* ========================================================================== */
/* Tests: modifier mask correctness for shifted characters                    */
/* ========================================================================== */

static void
test_modmask_ampersand_needs_shift(void)
{
	/*
	 * '&' (U+0026) must be at level 1 of some key in the US keymap,
	 * and the modifier mask for that level must include the Shift bit.
	 * This is the exact invariant that the send_text fix relies on.
	 */
	xkb_keysym_t ks;
	xkb_keycode_t kc;
	xkb_level_index_t level;

	ks = XKB_KEY_ampersand;
	kc = test_keysym_to_keycode(g_keymap, ks, NULL, &level);
	g_assert_cmpuint(kc, !=, 0);
	g_assert_cmpuint(level, ==, 1);
	g_assert_true(level1_uses_shift(g_keymap, kc));
}

static void
test_modmask_question_needs_shift(void)
{
	xkb_keysym_t ks;
	xkb_keycode_t kc;
	xkb_level_index_t level;

	ks = XKB_KEY_question;
	kc = test_keysym_to_keycode(g_keymap, ks, NULL, &level);
	g_assert_cmpuint(kc, !=, 0);
	g_assert_cmpuint(level, ==, 1);
	g_assert_true(level1_uses_shift(g_keymap, kc));
}

static void
test_modmask_colon_needs_shift(void)
{
	xkb_keysym_t ks;
	xkb_keycode_t kc;
	xkb_level_index_t level;

	ks = XKB_KEY_colon;
	kc = test_keysym_to_keycode(g_keymap, ks, NULL, &level);
	g_assert_cmpuint(kc, !=, 0);
	g_assert_cmpuint(level, ==, 1);
	g_assert_true(level1_uses_shift(g_keymap, kc));
}

static void
test_modmask_at_needs_shift(void)
{
	xkb_keysym_t ks;
	xkb_keycode_t kc;
	xkb_level_index_t level;

	ks = XKB_KEY_at;
	kc = test_keysym_to_keycode(g_keymap, ks, NULL, &level);
	g_assert_cmpuint(kc, !=, 0);
	g_assert_cmpuint(level, ==, 1);
	g_assert_true(level1_uses_shift(g_keymap, kc));
}

static void
test_modmask_exclamation_needs_shift(void)
{
	xkb_keysym_t ks;
	xkb_keycode_t kc;
	xkb_level_index_t level;

	ks = XKB_KEY_exclam;
	kc = test_keysym_to_keycode(g_keymap, ks, NULL, &level);
	g_assert_cmpuint(kc, !=, 0);
	g_assert_cmpuint(level, ==, 1);
	g_assert_true(level1_uses_shift(g_keymap, kc));
}

static void
test_modmask_hash_needs_shift(void)
{
	xkb_keysym_t ks;
	xkb_keycode_t kc;
	xkb_level_index_t level;

	ks = XKB_KEY_numbersign;
	kc = test_keysym_to_keycode(g_keymap, ks, NULL, &level);
	g_assert_cmpuint(kc, !=, 0);
	g_assert_cmpuint(level, ==, 1);
	g_assert_true(level1_uses_shift(g_keymap, kc));
}

static void
test_modmask_percent_needs_shift(void)
{
	xkb_keysym_t ks;
	xkb_keycode_t kc;
	xkb_level_index_t level;

	ks = XKB_KEY_percent;
	kc = test_keysym_to_keycode(g_keymap, ks, NULL, &level);
	g_assert_cmpuint(kc, !=, 0);
	g_assert_cmpuint(level, ==, 1);
	g_assert_true(level1_uses_shift(g_keymap, kc));
}

/*
 * test_modmask_shift_bit_resolves_to_shift_key:
 *
 * For a character that requires Shift (e.g. '&'), the modifier mask's
 * Shift bit must resolve via find_mod_key_evdev to a non-zero evdev code.
 * This tests the full pipeline: character → keysym → level → mod mask →
 * modifier bit → evdev keycode.
 */
static void
test_modmask_shift_bit_resolves_to_shift_key(void)
{
	xkb_keysym_t ks;
	xkb_keycode_t kc;
	xkb_layout_index_t layout;
	xkb_level_index_t level;
	xkb_mod_mask_t masks[8];
	size_t nmasks;
	gint bit;

	ks = XKB_KEY_ampersand;
	kc = test_keysym_to_keycode(g_keymap, ks, &layout, &level);
	g_assert_cmpuint(kc, !=, 0);
	g_assert_cmpuint(level, ==, 1);

	nmasks = xkb_keymap_key_get_mods_for_level(
		g_keymap, kc, layout, level, masks, G_N_ELEMENTS(masks));
	g_assert_cmpuint(nmasks, >, 0);

	/*
	 * For every set bit in the modifier mask, find_mod_key_evdev must
	 * return a non-zero evdev code (otherwise the key event cannot be
	 * sent and the character would be silently dropped).
	 */
	for (bit = 0; bit < (gint)(sizeof(xkb_mod_mask_t) * 8); bit++) {
		guint32 evdev;

		if (!(masks[0] & (1u << (guint)bit)))
			continue;

		evdev = test_find_mod_key_evdev(
			g_keymap, (xkb_mod_index_t)bit);
		g_assert_cmpuint(evdev, !=, 0);
	}
}

/* ========================================================================== */
/* Tests: URL character coverage                                               */
/* ========================================================================== */

/*
 * test_url_chars_coverage:
 *
 * Every character in a realistic URL must either:
 *   (a) be found in the keymap at level 0 or 1, OR
 *   (b) have no keysym (xkb_utf32_to_keysym returns XKB_KEY_NoSymbol)
 *       in which case send_text correctly skips it.
 *
 * No character should exist as a keysym but be missing from the keymap
 * (that would cause a silent drop without incrementing the skip counter).
 *
 * The URL exercises all special characters that triggered the bug:
 *   & ? : / = @ # % ! token/session values with mixed case.
 */
static void
test_url_chars_coverage(void)
{
	const gchar *url = "http://user@localhost:8080/auth/verify"
	                   "?token=AbCdEf123&session=XyZ!%40#value";
	const gchar *p;

	for (p = url; *p != '\0'; ) {
		gunichar uc;
		xkb_keysym_t ks;
		xkb_keycode_t kc;

		uc = g_utf8_get_char(p);
		p  = g_utf8_next_char(p);

		ks = xkb_utf32_to_keysym((guint32)uc);
		if (ks == XKB_KEY_NoSymbol)
			continue; /* send_text skips these — OK */

		/*
		 * If xkb_utf32_to_keysym returned a valid keysym, the
		 * keymap must contain it at some level.  Otherwise the
		 * character would be silently lost (not counted as skipped).
		 */
		kc = test_keysym_to_keycode(g_keymap, ks, NULL, NULL);
		g_assert_cmpuint(kc, !=, 0);
	}
}

/*
 * test_url_chars_only_level0_or_1:
 *
 * All typeable URL characters must live at level 0 or level 1.
 * Level > 1 would require a compound modifier (e.g. AltGr+Shift)
 * which is unusual on US QWERTY and not commonly present in URLs.
 * This ensures the fix covers the full URL character set.
 */
static void
test_url_chars_only_level0_or_1(void)
{
	/* Characters that must be typeable (not skipped) */
	const gchar chars[] =
		"abcdefghijklmnopqrstuvwxyz"
		"ABCDEFGHIJKLMNOPQRSTUVWXYZ"
		"0123456789"
		"!@#$%^&*()-_=+[]{}|;':\",./<>?`~"
		"/ =:@#&?%.+-";
	gsize i;

	for (i = 0; chars[i] != '\0'; i++) {
		gunichar uc;
		xkb_keysym_t ks;
		xkb_level_index_t level;
		xkb_keycode_t kc;

		uc = (gunichar)(guchar)chars[i];
		ks = xkb_utf32_to_keysym((guint32)uc);
		if (ks == XKB_KEY_NoSymbol)
			continue;

		kc = test_keysym_to_keycode(g_keymap, ks, NULL, &level);
		if (kc == 0)
			continue; /* not in this keymap — skip counter handles it */

		/* Level must be 0 or 1 — anything higher needs more logic */
		g_assert_cmpuint(level, <=, 1);
	}
}

/* ========================================================================== */
/* Tests: edge cases and robustness                                           */
/* ========================================================================== */

static void
test_edge_nul_codepoint(void)
{
	/*
	 * NUL (U+0000) should not be typeable.  Either xkb_utf32_to_keysym
	 * returns XKB_KEY_NoSymbol, or the keycode lookup returns 0.
	 * Either way, send_text must not crash.
	 */
	xkb_keysym_t ks;

	ks = xkb_utf32_to_keysym(0x0000u);
	/* If a keysym is returned, the keycode lookup must not crash */
	if (ks != XKB_KEY_NoSymbol) {
		xkb_keycode_t kc;

		kc = test_keysym_to_keycode(g_keymap, ks, NULL, NULL);
		/* kc may be 0 or non-zero — neither is a bug */
		(void)kc;
	}
}

static void
test_edge_control_chars_skipped(void)
{
	/*
	 * ASCII control characters (0x01–0x1F, excluding those with their
	 * own keysym like Tab and Return) must not produce a keycode that
	 * would send garbage into an application.
	 * Either they have no keysym, or they have a well-known one.
	 */
	static const guint32 ctrl_chars[] = {
		0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
		/* 0x08 = BackSpace, 0x09 = Tab, 0x0A = Linefeed — known */
		0x0B, 0x0C,
		/* 0x0D = Return — known */
		0x0E, 0x0F, 0x10, 0x11, 0x12, 0x13, 0x14,
		0x15, 0x16, 0x17, 0x18, 0x19, 0x1A,
		/* 0x1B = Escape — known */
		0x1C, 0x1D, 0x1E, 0x1F,
	};
	gsize i;

	for (i = 0; i < G_N_ELEMENTS(ctrl_chars); i++) {
		/*
		 * We don't assert a specific outcome — just that the lookup
		 * does not crash or produce an absurd evdev code.
		 */
		xkb_keysym_t ks;

		ks = xkb_utf32_to_keysym(ctrl_chars[i]);
		if (ks != XKB_KEY_NoSymbol) {
			xkb_keycode_t kc;

			kc = test_keysym_to_keycode(g_keymap, ks, NULL, NULL);
			/* If found, the evdev code must be sane (< 256) */
			if (kc != 0)
				g_assert_cmpuint(kc - 8, <, 256);
		}
	}
}

static void
test_edge_high_unicode_not_in_keymap(void)
{
	/*
	 * High-plane Unicode (e.g. emoji, U+1F600 = 😀) will likely not
	 * be in a standard US keymap.  xkb_utf32_to_keysym may return a
	 * keysym but keysym_to_keycode should return 0, causing send_text
	 * to skip the character (counted in skipped, not sent).
	 */
	xkb_keysym_t ks;
	xkb_keycode_t kc;

	ks = xkb_utf32_to_keysym(0x1F600u); /* U+1F600 = 😀 */
	/* ks might be non-zero but the keycode should be 0 on a US keymap */
	if (ks != XKB_KEY_NoSymbol) {
		kc = test_keysym_to_keycode(g_keymap, ks, NULL, NULL);
		g_assert_cmpuint(kc, ==, 0);
	}
}

static void
test_edge_char_level_helper_consistency(void)
{
	/*
	 * char_level() should agree with direct test_keysym_to_keycode().
	 * Cross-check for the most common URL characters.
	 */
	g_assert_cmpint(char_level(g_keymap, (gunichar)'a'), ==, 0);
	g_assert_cmpint(char_level(g_keymap, (gunichar)'A'), ==, 1);
	g_assert_cmpint(char_level(g_keymap, (gunichar)'&'), ==, 1);
	g_assert_cmpint(char_level(g_keymap, (gunichar)'?'), ==, 1);
	g_assert_cmpint(char_level(g_keymap, (gunichar)':'), ==, 1);
	g_assert_cmpint(char_level(g_keymap, (gunichar)'/'), ==, 0);
	g_assert_cmpint(char_level(g_keymap, (gunichar)'='), ==, 0);
	g_assert_cmpint(char_level(g_keymap, (gunichar)'-'), ==, 0);
	g_assert_cmpint(char_level(g_keymap, (gunichar)'.'), ==, 0);
	g_assert_cmpint(char_level(g_keymap, (gunichar)'0'), ==, 0);
	g_assert_cmpint(char_level(g_keymap, (gunichar)'9'), ==, 0);
}

/* ========================================================================== */
/* Main                                                                       */
/* ========================================================================== */

int
main(int argc, char *argv[])
{
	struct xkb_rule_names names;
	gint result;

	g_test_init(&argc, &argv, NULL);

	/* Set up a standard US QWERTY keymap shared by all tests */
	g_ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	g_assert_nonnull(g_ctx);

	memset(&names, 0, sizeof(names));
	names.model  = "pc105";
	names.layout = "us";
	g_keymap = xkb_keymap_new_from_names(g_ctx, &names,
		XKB_KEYMAP_COMPILE_NO_FLAGS);
	g_assert_nonnull(g_keymap);

	/* keysym_to_keycode */
	g_test_add_func("/mcp-input/keysym-to-keycode/level0-lowercase",
		test_ktk_level0_lowercase);
	g_test_add_func("/mcp-input/keysym-to-keycode/level1-uppercase",
		test_ktk_level1_uppercase);
	g_test_add_func("/mcp-input/keysym-to-keycode/url-unshifted",
		test_ktk_url_unshifted);
	g_test_add_func("/mcp-input/keysym-to-keycode/url-shifted",
		test_ktk_url_shifted);
	g_test_add_func("/mcp-input/keysym-to-keycode/modifier-keys-findable",
		test_ktk_modifier_keys_findable);
	g_test_add_func("/mcp-input/keysym-to-keycode/not-found-returns-zero",
		test_ktk_not_found_returns_zero);
	g_test_add_func("/mcp-input/keysym-to-keycode/idempotent",
		test_ktk_idempotent);
	g_test_add_func("/mcp-input/keysym-to-keycode/shift-l-evdev-correct",
		test_ktk_shift_l_evdev_correct);

	/* find_mod_key_evdev */
	g_test_add_func("/mcp-input/find-mod-key-evdev/shift-returns-leftshift",
		test_fmke_shift_returns_leftshift);
	g_test_add_func("/mcp-input/find-mod-key-evdev/ctrl-returns-nonzero",
		test_fmke_ctrl_returns_nonzero);
	g_test_add_func("/mcp-input/find-mod-key-evdev/alt-returns-nonzero",
		test_fmke_alt_returns_nonzero);
	g_test_add_func("/mcp-input/find-mod-key-evdev/invalid-index-returns-zero",
		test_fmke_invalid_index_returns_zero);
	g_test_add_func("/mcp-input/find-mod-key-evdev/known-mods-sane-evdev",
		test_fmke_known_mods_sane_evdev);

	/* xkb_utf32_to_keysym for URL-special characters */
	g_test_add_func("/mcp-input/utf32-to-keysym/ampersand",
		test_utf32_ampersand);
	g_test_add_func("/mcp-input/utf32-to-keysym/question",
		test_utf32_question);
	g_test_add_func("/mcp-input/utf32-to-keysym/colon",
		test_utf32_colon);
	g_test_add_func("/mcp-input/utf32-to-keysym/slash",
		test_utf32_slash);
	g_test_add_func("/mcp-input/utf32-to-keysym/at",
		test_utf32_at);
	g_test_add_func("/mcp-input/utf32-to-keysym/equals",
		test_utf32_equals);
	g_test_add_func("/mcp-input/utf32-to-keysym/hash",
		test_utf32_hash);
	g_test_add_func("/mcp-input/utf32-to-keysym/percent",
		test_utf32_percent);

	/* Modifier mask correctness */
	g_test_add_func("/mcp-input/modifier-mask/ampersand-needs-shift",
		test_modmask_ampersand_needs_shift);
	g_test_add_func("/mcp-input/modifier-mask/question-needs-shift",
		test_modmask_question_needs_shift);
	g_test_add_func("/mcp-input/modifier-mask/colon-needs-shift",
		test_modmask_colon_needs_shift);
	g_test_add_func("/mcp-input/modifier-mask/at-needs-shift",
		test_modmask_at_needs_shift);
	g_test_add_func("/mcp-input/modifier-mask/exclamation-needs-shift",
		test_modmask_exclamation_needs_shift);
	g_test_add_func("/mcp-input/modifier-mask/hash-needs-shift",
		test_modmask_hash_needs_shift);
	g_test_add_func("/mcp-input/modifier-mask/percent-needs-shift",
		test_modmask_percent_needs_shift);
	g_test_add_func(
		"/mcp-input/modifier-mask/shift-bit-resolves-to-shift-key",
		test_modmask_shift_bit_resolves_to_shift_key);

	/* URL character coverage */
	g_test_add_func("/mcp-input/url-coverage/all-chars-in-keymap",
		test_url_chars_coverage);
	g_test_add_func("/mcp-input/url-coverage/only-level0-or-1",
		test_url_chars_only_level0_or_1);

	/* Edge cases */
	g_test_add_func("/mcp-input/edge/nul-codepoint",
		test_edge_nul_codepoint);
	g_test_add_func("/mcp-input/edge/control-chars-skipped",
		test_edge_control_chars_skipped);
	g_test_add_func("/mcp-input/edge/high-unicode-not-in-keymap",
		test_edge_high_unicode_not_in_keymap);
	g_test_add_func("/mcp-input/edge/char-level-helper-consistency",
		test_edge_char_level_helper_consistency);

	result = g_test_run();

	xkb_keymap_unref(g_keymap);
	xkb_context_unref(g_ctx);

	return result;
}
