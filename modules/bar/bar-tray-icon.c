/*
 * gowl - GObject Wayland Compositor
 * Copyright (C) 2026  Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Turning what a tray item says about its icon into pixels.
 *
 * An item offers up to three things and any of them may be the only one
 * it has: raw ARGB pixmaps, a themed icon name, and a directory of its
 * own to look that name up in.  Applications are inconsistent about
 * which they provide and wrong about the rest often enough that a tray
 * which handles only the documented path shows blanks for a third of
 * what is running.
 *
 * WHY THE THEME LOOKUP IS DONE BY HAND.  Resolving a themed name
 * properly is GtkIconTheme's job, and linking GTK into a bar widget to
 * find a PNG is a lot of process for the question.  What is here is the
 * icon theme specification's directory layout walked directly --- which
 * is all that is needed, because a tray icon is one name at one size and
 * the answer is cached for the life of the item.
 */

#include "bar-tray-icon.h"

#include <string.h>

#include <gdk-pixbuf/gdk-pixbuf.h>

/* Themes to try, in order, when an item does not bring its own
 * directory.  hicolor last because it is the fallback every theme
 * inherits from and is where an application's own install lands. */
static const gchar *const icon_themes[] = {
	"Adwaita", "breeze", "Papirus", "gnome", "hicolor", NULL
};

static const gchar *const icon_exts[] = { "png", "svg", "xpm", NULL };

typedef struct {
	gchar *path;
	gint   size;     /* what the directory claims, 0 for scalable */
} Candidate;

/*
 * How good a directory's size is for what is wanted.
 *
 * Exact wins.  Then anything BIGGER, smallest first: scaling a 32 down
 * to 22 is sharp and scaling a 16 up to 22 is mush.  Scalable counts as
 * infinitely big and therefore as the last good answer, which is right:
 * an SVG rasterised at the target is better than a small PNG stretched,
 * but a 24 PNG is better still and costs nothing to draw.
 */
static gint
size_score(gint have, gint want)
{
	if (have == want)
		return 0;
	if (have == 0)
		return 10000;               /* scalable */
	if (have > want)
		return have - want;
	return 20000 + (want - have);   /* smaller: only if nothing else */
}

/* "48x48" or "48" -> 48; "scalable" -> 0; anything else -> -1. */
static gint
dir_size(const gchar *name)
{
	gchar *end = NULL;
	gint64 v;

	if (g_strcmp0(name, "scalable") == 0)
		return 0;
	v = g_ascii_strtoll(name, &end, 10);
	if (end == name || v <= 0 || v > 4096)
		return -1;
	if (*end != '\0' && *end != 'x')
		return -1;
	return (gint)v;
}

static gboolean
file_is_icon(const gchar *name, const gchar *want)
{
	guint i;

	for (i = 0; icon_exts[i] != NULL; i++) {
		g_autofree gchar *candidate =
			g_strconcat(want, ".", icon_exts[i], NULL);

		if (g_strcmp0(name, candidate) == 0)
			return TRUE;
	}
	return FALSE;
}

/*
 * Walk a directory tree for `<name>.<ext>', remembering the best size
 * seen.  Depth-limited: an application's own icon directory is two or
 * three levels and an icon theme is exactly three, so a limit keeps a
 * mistaken `IconThemePath' of "/" from walking the disk.
 */
static void
search_tree(const gchar *dir, const gchar *name, gint want, gint depth,
            Candidate *best, gint claimed_size)
{
	g_autoptr(GDir) d = NULL;
	const gchar *entry;

	if (depth < 0)
		return;
	d = g_dir_open(dir, 0, NULL);
	if (d == NULL)
		return;

	while ((entry = g_dir_read_name(d)) != NULL) {
		g_autofree gchar *path = g_build_filename(dir, entry, NULL);

		if (g_file_test(path, G_FILE_TEST_IS_DIR)) {
			gint s = dir_size(entry);

			/* A directory named for a size says what is under it;
			 * anything else keeps whatever the parent claimed. */
			search_tree(path, name, want, depth - 1, best,
			            s >= 0 ? s : claimed_size);
			continue;
		}
		if (!file_is_icon(entry, name))
			continue;
		if (best->path == NULL
		    || size_score(claimed_size, want)
		       < size_score(best->size, want)) {
			g_free(best->path);
			best->path = g_strdup(path);
			best->size = claimed_size;
		}
	}
}

/* Every base directory the icon theme specification names. */
static GPtrArray *
icon_base_dirs(void)
{
	GPtrArray *dirs = g_ptr_array_new_with_free_func(g_free);
	const gchar *const *data_dirs;
	guint i;

	g_ptr_array_add(dirs, g_build_filename(g_get_home_dir(), ".icons", NULL));
	g_ptr_array_add(dirs, g_build_filename(g_get_user_data_dir(),
	                                       "icons", NULL));
	data_dirs = g_get_system_data_dirs();
	for (i = 0; data_dirs != NULL && data_dirs[i] != NULL; i++)
		g_ptr_array_add(dirs, g_build_filename(data_dirs[i], "icons", NULL));
	/* Not a theme directory at all, and where a surprising number of
	 * applications still install their tray icon. */
	g_ptr_array_add(dirs, g_strdup("/usr/share/pixmaps"));
	return dirs;
}

static gchar *
find_icon_file(const gchar *name, const gchar *theme_path, gint want)
{
	Candidate best = { NULL, 0 };
	g_autoptr(GPtrArray) bases = NULL;
	guint i, t;

	if (name == NULL || *name == '\0')
		return NULL;

	/* An absolute path is not a name, and several applications send
	 * one.  Taking it at face value is both correct and much cheaper
	 * than searching for a file whose full path we were handed. */
	if (g_path_is_absolute(name)) {
		if (g_file_test(name, G_FILE_TEST_IS_REGULAR))
			return g_strdup(name);
		return NULL;
	}

	/* The item's own directory first: it is there precisely because the
	 * theme has never heard of this icon. */
	if (theme_path != NULL && *theme_path != '\0') {
		g_auto(GStrv) parts = g_strsplit(theme_path, ":", -1);

		for (i = 0; parts[i] != NULL; i++) {
			if (parts[i][0] != '\0')
				search_tree(parts[i], name, want, 3, &best, want);
		}
		if (best.path != NULL)
			return best.path;
	}

	bases = icon_base_dirs();
	for (t = 0; icon_themes[t] != NULL; t++) {
		for (i = 0; i < bases->len; i++) {
			g_autofree gchar *dir = g_build_filename(
				g_ptr_array_index(bases, i), icon_themes[t], NULL);

			search_tree(dir, name, want, 3, &best, -1);
		}
		if (best.path != NULL)
			return best.path;
	}

	/* And the base directories themselves, for /usr/share/pixmaps and
	 * anything else that ignores the theme layout. */
	for (i = 0; i < bases->len; i++)
		search_tree(g_ptr_array_index(bases, i), name, want, 1, &best, -1);
	return best.path;
}

/* ── Pixmaps ─────────────────────────────────────────────────────── */

/* Scale onto a square of @size, keeping the aspect and centring. */
static cairo_surface_t *
scale_to(cairo_surface_t *src, gint size)
{
	cairo_surface_t *out;
	cairo_t *cr;
	gint sw, sh;
	gdouble scale;

	if (src == NULL || size <= 0)
		return NULL;
	sw = cairo_image_surface_get_width(src);
	sh = cairo_image_surface_get_height(src);
	if (sw <= 0 || sh <= 0)
		return NULL;
	if (sw == size && sh == size)
		return cairo_surface_reference(src);

	out = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, size, size);
	cr = cairo_create(out);
	scale = MIN((gdouble)size / sw, (gdouble)size / sh);
	cairo_translate(cr, (size - sw * scale) * 0.5, (size - sh * scale) * 0.5);
	cairo_scale(cr, scale, scale);
	cairo_set_source_surface(cr, src, 0, 0);
	cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
	cairo_paint(cr);
	cairo_destroy(cr);
	return out;
}

static cairo_surface_t *
surface_from_file(const gchar *path, gint size)
{
	g_autoptr(GdkPixbuf) pixbuf = NULL;
	cairo_surface_t *surface, *scaled;
	guint8 *dst;
	gint w, h, stride, x, y, n_channels, rowstride;
	const guint8 *pixels;

	if (path == NULL)
		return NULL;
	/* Asked for at the target size, which is what makes an SVG
	 * rasterise sharp rather than be drawn at its own size and
	 * resampled. */
	pixbuf = gdk_pixbuf_new_from_file_at_scale(path, size, size, TRUE, NULL);
	if (pixbuf == NULL)
		return NULL;

	w = gdk_pixbuf_get_width(pixbuf);
	h = gdk_pixbuf_get_height(pixbuf);
	n_channels = gdk_pixbuf_get_n_channels(pixbuf);
	rowstride = gdk_pixbuf_get_rowstride(pixbuf);
	pixels = gdk_pixbuf_get_pixels(pixbuf);
	if (w <= 0 || h <= 0 || (n_channels != 3 && n_channels != 4))
		return NULL;

	surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
	dst = cairo_image_surface_get_data(surface);
	stride = cairo_image_surface_get_stride(surface);
	for (y = 0; y < h; y++) {
		for (x = 0; x < w; x++) {
			const guint8 *s = pixels + (gsize)y * rowstride
			                + (gsize)x * n_channels;
			guint32 *d = (guint32 *)(dst + (gsize)y * stride) + x;
			guint a = (n_channels == 4) ? s[3] : 255;
			guint r = (s[0] * a + 127) / 255;
			guint g = (s[1] * a + 127) / 255;
			guint b = (s[2] * a + 127) / 255;

			*d = ((guint32)a << 24) | (r << 16) | (g << 8) | b;
		}
	}
	cairo_surface_mark_dirty(surface);

	scaled = scale_to(surface, size);
	cairo_surface_destroy(surface);
	return scaled;
}

/* ── The cache ───────────────────────────────────────────────────── */

GHashTable *
bar_tray_icon_cache_new(void)
{
	return g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
		(GDestroyNotify)cairo_surface_destroy);
}

cairo_surface_t *
bar_tray_icon_for(GHashTable *cache, const GowlTrayItem *item, gint size)
{
	g_autofree gchar *cache_key = NULL;
	cairo_surface_t *surface = NULL;
	const gchar *name;

	if (cache == NULL || item == NULL || size <= 0)
		return NULL;

	/*
	 * Keyed on the item's serial as well as its identity, so an
	 * application that changes its icon -- which is the entire point of
	 * a tray icon for a syncing or connecting application -- is not
	 * served its old one for ever.
	 */
	cache_key = g_strdup_printf("%s\x1f%u\x1f%d", item->key, item->serial,
	                            size);
	if (g_hash_table_lookup_extended(cache, cache_key, NULL,
	                                 (gpointer *)&surface))
		return surface;

	/* An item in NeedsAttention says so with a different icon. */
	name = item->icon_name;
	if (g_strcmp0(item->status, "NeedsAttention") == 0
	    && item->attention_icon_name != NULL
	    && item->attention_icon_name[0] != '\0')
		name = item->attention_icon_name;

	/*
	 * The pixmap first.  It is what the application chose to hand over
	 * rather than a name to look up, so it cannot be missing, cannot be
	 * the wrong theme's, and is the only one that carries a status the
	 * application renders into the icon itself.
	 */
	if (item->pixmap != NULL) {
		cairo_surface_t *raw = gowl_tray_item_argb32(item);

		if (raw != NULL) {
			surface = scale_to(raw, size);
			cairo_surface_destroy(raw);
		}
	}

	if (surface == NULL && name != NULL) {
		g_autofree gchar *path =
			find_icon_file(name, item->icon_theme_path, size);

		surface = surface_from_file(path, size);
	}

	/* Last: the application's own Id as an icon name.  A guess, and a
	 * good one -- an application's tray icon is very often installed
	 * under the same name the application calls itself. */
	if (surface == NULL && item->id != NULL) {
		g_autofree gchar *lower = g_ascii_strdown(item->id, -1);
		g_autofree gchar *path =
			find_icon_file(lower, item->icon_theme_path, size);

		surface = surface_from_file(path, size);
	}

	/* Inserted even when NULL, so a name that cannot be resolved is
	 * searched for once rather than on every repaint. */
	g_hash_table_insert(cache, g_steal_pointer(&cache_key), surface);
	return surface;
}
