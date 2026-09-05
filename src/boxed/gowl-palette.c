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
 * gowl-palette.c -- one set of colours for the whole session
 *
 * Before this, a single gowl session carried five independent palettes:
 * three border colours in dwm's blue, a twelve-key colour block in
 * gowlbar, the lock screen's four, the terminal's, and the editor's.
 * Changing "the" colour of the desktop meant editing all of them, so
 * nobody did, and they had already drifted --- the shipped config's
 * borders were dwm's #005577 while its bar was Catppuccin.
 *
 * A palette is a flat name-to-literal map.  Entries cannot reference
 * other entries: resolution is one lookup with no cycle to detect, and
 * a palette can be printed, diffed and pushed over D-Bus as-is.
 *
 * The semantic names are Catppuccin's, because that is what the shipped
 * config was already half-using and what the user's other tools use.
 * They are only names, though --- `latte' is the same key set with
 * light values, and a user's `palette:' block is free to define
 * whatever it likes.
 */

#include "gowl-palette.h"

#include <string.h>

struct _GowlPalette {
	GHashTable *entries;    /* name -> hex, both owned */
};

G_DEFINE_BOXED_TYPE(GowlPalette, gowl_palette,
                    gowl_palette_copy, gowl_palette_free)

/* ── Built-in palettes ───────────────────────────────────────────── */

typedef struct {
	const gchar *name;
	const gchar *value;
} GowlPaletteEntry;

/*
 * Every built-in defines the same key set, so a config written against
 * one flavour renders under any other.  A palette missing a key a
 * config references is not an error --- the name resolves to itself and
 * the consumer sees an unparseable colour --- but it is a bad
 * experience, so the flavours are kept parallel by hand.
 */
#define GOWL_PALETTE_KEYS(base, mantle, crust, surface, overlay, \
                          text, subtext, accent, red, green, \
                          yellow, blue, mauve, teal, peach) \
	{ "base",    base    }, \
	{ "mantle",  mantle  }, \
	{ "crust",   crust   }, \
	{ "surface", surface }, \
	{ "overlay", overlay }, \
	{ "text",    text    }, \
	{ "subtext", subtext }, \
	{ "accent",  accent  }, \
	{ "red",     red     }, \
	{ "green",   green   }, \
	{ "yellow",  yellow  }, \
	{ "blue",    blue    }, \
	{ "mauve",   mauve   }, \
	{ "teal",    teal    }, \
	{ "peach",   peach   }

static const GowlPaletteEntry palette_mocha[] = {
	GOWL_PALETTE_KEYS("#1e1e2e", "#181825", "#11111b", "#313244",
	                  "#6c7086", "#cdd6f4", "#a6adc8", "#89b4fa",
	                  "#f38ba8", "#a6e3a1", "#f9e2af", "#89b4fa",
	                  "#cba6f7", "#94e2d5", "#fab387"),
	{ NULL, NULL }
};

static const GowlPaletteEntry palette_macchiato[] = {
	GOWL_PALETTE_KEYS("#24273a", "#1e2030", "#181926", "#363a4f",
	                  "#6e738d", "#cad3f5", "#b8c0e0", "#8aadf4",
	                  "#ed8796", "#a6da95", "#eed49f", "#8aadf4",
	                  "#c6a0f6", "#8bd5ca", "#f5a97f"),
	{ NULL, NULL }
};

static const GowlPaletteEntry palette_frappe[] = {
	GOWL_PALETTE_KEYS("#303446", "#292c3c", "#232634", "#414559",
	                  "#737994", "#c6d0f5", "#b5bfe2", "#8caaee",
	                  "#e78284", "#a6d189", "#e5c890", "#8caaee",
	                  "#ca9ee6", "#81c8be", "#ef9f76"),
	{ NULL, NULL }
};

static const GowlPaletteEntry palette_latte[] = {
	GOWL_PALETTE_KEYS("#eff1f5", "#e6e9ef", "#dce0e8", "#ccd0da",
	                  "#9ca0b0", "#4c4f69", "#6c6f85", "#1e66f5",
	                  "#d20f39", "#40a02b", "#df8e1d", "#1e66f5",
	                  "#8839ef", "#179299", "#fe640b"),
	{ NULL, NULL }
};

/*
 * What gowl looked like before palettes.  Kept so `palette: {name: dwm}'
 * restores the previous appearance exactly, rather than leaving anyone
 * who liked it to reconstruct three hex values from a changelog.
 */
static const GowlPaletteEntry palette_dwm[] = {
	GOWL_PALETTE_KEYS("#222222", "#111111", "#000000", "#444444",
	                  "#444444", "#bbbbbb", "#888888", "#005577",
	                  "#ff0000", "#00aa00", "#aaaa00", "#005577",
	                  "#770055", "#007777", "#aa5500"),
	{ NULL, NULL }
};

static const struct {
	const gchar            *name;
	const GowlPaletteEntry *entries;
} builtins[] = {
	{ "mocha",     palette_mocha     },
	{ "macchiato", palette_macchiato },
	{ "frappe",    palette_frappe    },
	{ "latte",     palette_latte     },
	{ "dwm",       palette_dwm       },
};

const gchar * const *
gowl_palette_builtin_names(void)
{
	static const gchar *names[G_N_ELEMENTS(builtins) + 1];
	static gsize once = 0;

	if (g_once_init_enter(&once)) {
		gsize i;

		for (i = 0; i < G_N_ELEMENTS(builtins); i++)
			names[i] = builtins[i].name;
		names[G_N_ELEMENTS(builtins)] = NULL;

		g_once_init_leave(&once, 1);
	}

	return names;
}

/* ── Lifecycle ───────────────────────────────────────────────────── */

GowlPalette *
gowl_palette_new(void)
{
	GowlPalette *self;

	self = g_new0(GowlPalette, 1);
	self->entries = g_hash_table_new_full(g_str_hash, g_str_equal,
	                                      g_free, g_free);
	return self;
}

GowlPalette *
gowl_palette_new_builtin(const gchar *name)
{
	GowlPalette *self;
	const GowlPaletteEntry *entries = palette_mocha;
	gsize i;

	self = gowl_palette_new();

	if (name != NULL) {
		for (i = 0; i < G_N_ELEMENTS(builtins); i++) {
			if (g_ascii_strcasecmp(builtins[i].name, name) == 0) {
				entries = builtins[i].entries;
				break;
			}
		}
	}

	for (i = 0; entries[i].name != NULL; i++)
		gowl_palette_set(self, entries[i].name, entries[i].value);

	return self;
}

GowlPalette *
gowl_palette_copy(const GowlPalette *self)
{
	GowlPalette *copy;
	GHashTableIter iter;
	gpointer key, value;

	if (self == NULL)
		return NULL;

	copy = gowl_palette_new();

	g_hash_table_iter_init(&iter, self->entries);
	while (g_hash_table_iter_next(&iter, &key, &value)) {
		g_hash_table_insert(copy->entries,
		                    g_strdup((const gchar *)key),
		                    g_strdup((const gchar *)value));
	}

	return copy;
}

void
gowl_palette_free(GowlPalette *self)
{
	if (self == NULL)
		return;

	g_hash_table_unref(self->entries);
	g_free(self);
}

/* ── Entries ─────────────────────────────────────────────────────── */

void
gowl_palette_set(GowlPalette *self, const gchar *name, const gchar *hex)
{
	g_return_if_fail(self != NULL);
	g_return_if_fail(name != NULL);

	if (hex == NULL) {
		g_hash_table_remove(self->entries, name);
		return;
	}

	g_hash_table_insert(self->entries, g_strdup(name), g_strdup(hex));
}

const gchar *
gowl_palette_lookup(const GowlPalette *self, const gchar *name)
{
	if (self == NULL || name == NULL)
		return NULL;

	return (const gchar *)g_hash_table_lookup(self->entries, name);
}

guint
gowl_palette_size(const GowlPalette *self)
{
	if (self == NULL)
		return 0;

	return g_hash_table_size(self->entries);
}

gchar **
gowl_palette_names(const GowlPalette *self)
{
	GPtrArray *out;
	GList *keys, *l;

	out = g_ptr_array_new();

	if (self != NULL) {
		/* Sorted, so that anything printing a palette --- the
		 * generated config, a D-Bus reply, a test --- is stable
		 * across runs regardless of hash order. */
		keys = g_hash_table_get_keys(self->entries);
		keys = g_list_sort(keys, (GCompareFunc)g_strcmp0);
		for (l = keys; l != NULL; l = l->next)
			g_ptr_array_add(out, g_strdup((const gchar *)l->data));
		g_list_free(keys);
	}

	g_ptr_array_add(out, NULL);
	return (gchar **)g_ptr_array_free(out, FALSE);
}

void
gowl_palette_merge(GowlPalette *self, const GowlPalette *other)
{
	GHashTableIter iter;
	gpointer key, value;

	g_return_if_fail(self != NULL);

	if (other == NULL)
		return;

	g_hash_table_iter_init(&iter, other->entries);
	while (g_hash_table_iter_next(&iter, &key, &value)) {
		g_hash_table_insert(self->entries,
		                    g_strdup((const gchar *)key),
		                    g_strdup((const gchar *)value));
	}
}

/* ── Resolution ──────────────────────────────────────────────────── */

gchar *
gowl_palette_resolve(const GowlPalette *self, const gchar *spec)
{
	const gchar *slash;
	const gchar *hex;
	g_autofree gchar *name = NULL;

	if (spec == NULL)
		return NULL;

	/* A literal passes straight through.  Checked first so a palette
	 * that happens to contain an entry named "#1e1e2e" --- which is
	 * legal, if strange --- cannot shadow the literal. */
	if (spec[0] == '#')
		return g_strdup(spec);

	slash = strchr(spec, '/');
	if (slash == NULL) {
		hex = gowl_palette_lookup(self, spec);
		return g_strdup(hex != NULL ? hex : spec);
	}

	/* name/aa --- the entry with its alpha replaced. */
	name = g_strndup(spec, (gsize)(slash - spec));
	hex = gowl_palette_lookup(self, name);
	if (hex == NULL)
		return g_strdup(spec);

	/* Two hex digits, and nothing else.  A malformed alpha is returned
	 * unchanged rather than silently dropped, so it shows up as a
	 * broken colour the user can see instead of an opaque one they
	 * cannot distinguish from a config that did not apply. */
	if (strlen(slash + 1) != 2
	    || !g_ascii_isxdigit(slash[1]) || !g_ascii_isxdigit(slash[2]))
		return g_strdup(spec);

	/* Take the RGB of the entry, whether or not it already carried an
	 * alpha of its own, and append the requested one. */
	if (strlen(hex) >= 7)
		return g_strdup_printf("%.7s%c%c", hex, slash[1], slash[2]);

	return g_strdup(spec);
}

gboolean
gowl_palette_key_is_color(const gchar *key)
{
	if (key == NULL)
		return FALSE;

	return strstr(key, "color") != NULL || strstr(key, "colour") != NULL;
}
