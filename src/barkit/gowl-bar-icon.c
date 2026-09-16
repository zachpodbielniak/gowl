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
 * The icon theme specification's directory layout, walked directly.
 *
 * Extracted from the bar's tray widget, where it lived first and where
 * it was never really about trays.  See gowl-bar-icon.h.
 */

#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "gowl-bar-icon"

#include "barkit/gowl-bar-icon.h"

#include <string.h>
#include <stdlib.h>

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


/* ── The index ───────────────────────────────────────────────────── */

/*
 * One walk of the icon themes, shared by every name.
 *
 * find_icon_file() below walks the theme trees looking for one file.
 * That is fine for the five icons a tray shows once and catastrophic
 * for a menu: measured on a normal Fedora install it is 100--185ms PER
 * NAME, because Adwaita alone is hundreds of directories, and a name no
 * theme has -- there are always a few -- walks all of them before
 * giving up.  A dozen application rows cost a second and a half.
 *
 * So the directories are enumerated ONCE into a table of name -> best
 * file, and a lookup is then a hash probe.  This is what GtkIconTheme
 * does and for the same reason.
 *
 * The index is built per SIZE, because which directory wins depends on
 * the size wanted, and there are only ever one or two sizes in play.
 * It is not invalidated: an icon installed while the session runs is
 * not seen until it restarts, which is the same bargain the per-name
 * cache already made and is invisible next to not stalling the desktop.
 */

typedef struct {
	GHashTable *by_name;   /* name -> Candidate *, owned */
} IconIndex;

static void
candidate_free(gpointer data)
{
	Candidate *c = data;

	if (c == NULL)
		return;
	g_free(c->path);
	g_free(c);
}

/* Fold one directory's files into the index, keeping the best size. */
static void
index_dir(GHashTable *by_name, const gchar *dir, gint want, gint claimed,
          gint depth)
{
	g_autoptr(GDir) handle = NULL;
	const gchar *entry;

	if (depth < 0)
		return;
	handle = g_dir_open(dir, 0, NULL);
	if (handle == NULL)
		return;

	while ((entry = g_dir_read_name(handle)) != NULL) {
		g_autofree gchar *path = g_build_filename(dir, entry, NULL);

		if (g_file_test(path, G_FILE_TEST_IS_DIR)) {
			gint size = claimed;

			/* A directory named for its size says so; `scalable'
			 * says it has no size at all. */
			if (g_str_has_prefix(entry, "scalable"))
				size = 0;
			else if (g_ascii_isdigit(entry[0]))
				size = atoi(entry);
			index_dir(by_name, path, want, size, depth - 1);
			continue;
		}

		{
			const gchar *dot = strrchr(entry, '.');
			g_autofree gchar *name = NULL;
			Candidate *have;
			gint e;

			if (dot == NULL)
				continue;
			for (e = 0; icon_exts[e] != NULL; e++) {
				if (g_ascii_strcasecmp(dot + 1, icon_exts[e]) == 0)
					break;
			}
			if (icon_exts[e] == NULL)
				continue;

			name = g_strndup(entry, (gsize)(dot - entry));
			have = g_hash_table_lookup(by_name, name);
			if (have != NULL
			    && size_score(have->size, want)
			       <= size_score(claimed, want))
				continue;

			{
				Candidate *c = g_new0(Candidate, 1);

				c->path = g_steal_pointer(&path);
				c->size = claimed;
				g_hash_table_replace(by_name,
					g_steal_pointer(&name), c);
			}
		}
	}
}

/*
 * The index is process-wide and reached from more than one thread: the
 * bar's tray widget resolves on the compositor thread and the menu
 * resolves on a worker, precisely so the walk below cannot stall
 * compositing.  One lock over the whole build is right -- it is taken
 * once per size and held for the only part that is slow.
 */
G_LOCK_DEFINE_STATIC(icon_index);

static IconIndex *
icon_index_for(gint want)
{
	static GHashTable *by_size;     /* size -> IconIndex * */
	IconIndex *index;
	g_autoptr(GPtrArray) bases = NULL;
	guint i, t;

	G_LOCK(icon_index);

	if (by_size == NULL)
		by_size = g_hash_table_new(g_direct_hash, g_direct_equal);

	index = g_hash_table_lookup(by_size, GINT_TO_POINTER(want));
	if (index != NULL) {
		G_UNLOCK(icon_index);
		return index;
	}

	index = g_new0(IconIndex, 1);
	index->by_name = g_hash_table_new_full(g_str_hash, g_str_equal,
	                                       g_free, candidate_free);

	/*
	 * Themes in REVERSE order, so an earlier theme's entry overwrites
	 * a later one's: index_dir() keeps whichever it sees last at an
	 * equal size, and the order in icon_themes[] is the order of
	 * preference.
	 */
	bases = icon_base_dirs();
	for (t = 0; icon_themes[t] != NULL; t++)
		;
	while (t-- > 0) {
		for (i = 0; i < bases->len; i++) {
			g_autofree gchar *dir = g_build_filename(
				g_ptr_array_index(bases, i), icon_themes[t],
				NULL);

			index_dir(index->by_name, dir, want, 0, 3);
		}
	}
	/* And the base directories themselves, for /usr/share/pixmaps and
	 * anything else that ignores the theme layout.  Lowest priority,
	 * so first. */
	for (i = 0; i < bases->len; i++) {
		index_dir(index->by_name, g_ptr_array_index(bases, i), want,
		          0, 0);
	}

	g_hash_table_insert(by_size, GINT_TO_POINTER(want), index);
	G_UNLOCK(icon_index);
	return index;
}

static gchar *
find_icon_file(const gchar *name, const gchar *theme_path, gint want)
{
	Candidate best = { NULL, 0 };
	guint i;

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

	/* The index, which is one walk of every theme shared by every
	 * name rather than a walk per name. */
	{
		IconIndex *index = icon_index_for(want);
		Candidate *hit;
		gchar *found = NULL;

		/* Copied out under the lock: an index is never mutated
		 * after it is built, but the table it is looked up in can
		 * gain another size's while this runs. */
		G_LOCK(icon_index);
		hit = g_hash_table_lookup(index->by_name, name);
		if (hit != NULL)
			found = g_strdup(hit->path);
		G_UNLOCK(icon_index);
		return found;
	}
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
gowl_bar_icon_cache_new(void)
{
	return g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
		(GDestroyNotify)cairo_surface_destroy);
}

gchar *
gowl_bar_icon_find_file(const gchar *name, const gchar *theme_path, gint size)
{
	return find_icon_file(name, theme_path, size);
}

cairo_surface_t *
gowl_bar_icon_from_file(const gchar *path, gint size)
{
	return surface_from_file(path, size);
}

cairo_surface_t *
gowl_bar_icon_scale(cairo_surface_t *src, gint size)
{
	return scale_to(src, size);
}

/*
 * How far two channels may differ and still count as the same tone.
 * Scaling a glyph resamples its edges, so even a pixmap that started
 * out perfectly flat comes back with a few pixels a shade off.
 */
#define GOWL_BAR_ICON_MASK_SLACK (12)

/* Pixels this faint are the glyph's antialiased fringe and say nothing
 * about its colour. */
#define GOWL_BAR_ICON_MASK_FLOOR (24)

gboolean
gowl_bar_icon_is_mask(cairo_surface_t *surface)
{
	const guint8 *data;
	gint w, h, stride, x, y;
	gint lo = 256, hi = -1;
	gboolean seen = FALSE;

	if (surface == NULL
	    || cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS
	    || cairo_image_surface_get_format(surface) != CAIRO_FORMAT_ARGB32)
		return FALSE;

	data   = cairo_image_surface_get_data(surface);
	w      = cairo_image_surface_get_width(surface);
	h      = cairo_image_surface_get_height(surface);
	stride = cairo_image_surface_get_stride(surface);
	if (data == NULL || w <= 0 || h <= 0)
		return FALSE;

	for (y = 0; y < h; y++) {
		const guint32 *row = (const guint32 *)(data + (gsize)y * stride);

		for (x = 0; x < w; x++) {
			guint32 px = row[x];
			gint a = (gint)((px >> 24) & 0xff);
			gint r, g, b, cmin, cmax;

			if (a < GOWL_BAR_ICON_MASK_FLOOR)
				continue;

			/* Cairo's ARGB32 is premultiplied; undo it, or a
			 * half-transparent white reads as a grey and every
			 * antialiased edge looks like a second colour. */
			r = (gint)(((px >> 16) & 0xff) * 255 / a);
			g = (gint)(((px >>  8) & 0xff) * 255 / a);
			b = (gint)(( px        & 0xff) * 255 / a);
			if (r > 255) r = 255;
			if (g > 255) g = 255;
			if (b > 255) b = 255;

			/*
			 * One tone, across every channel and every pixel.
			 * Tracking the extremes of the channels themselves
			 * answers both questions at once: a pixel with any
			 * chroma in it spreads the range as surely as a
			 * second shade does, so a purple icon and a
			 * greyscale photograph are both rejected here, and
			 * both should be --- the first has a colour of its
			 * own and the second would lose its shading.
			 */
			cmin = MIN(r, MIN(g, b));
			cmax = MAX(r, MAX(g, b));
			if (cmax > hi) hi = cmax;
			if (cmin < lo) lo = cmin;
			if (hi - lo > GOWL_BAR_ICON_MASK_SLACK)
				return FALSE;

			seen = TRUE;
		}
	}

	/* A fully transparent image is not a mask of anything. */
	return seen;
}

cairo_surface_t *
gowl_bar_icon_tint(cairo_surface_t *surface, gdouble r, gdouble g, gdouble b)
{
	cairo_surface_t *out;
	const guint8 *src;
	guint8 *dst;
	gint w, h, src_stride, dst_stride, x, y;
	gint ri, gi, bi;

	if (surface == NULL
	    || cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS
	    || cairo_image_surface_get_format(surface) != CAIRO_FORMAT_ARGB32)
		return NULL;

	w = cairo_image_surface_get_width(surface);
	h = cairo_image_surface_get_height(surface);
	if (w <= 0 || h <= 0)
		return NULL;

	out = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
	if (cairo_surface_status(out) != CAIRO_STATUS_SUCCESS) {
		cairo_surface_destroy(out);
		return NULL;
	}

	ri = (gint)(CLAMP(r, 0.0, 1.0) * 255.0 + 0.5);
	gi = (gint)(CLAMP(g, 0.0, 1.0) * 255.0 + 0.5);
	bi = (gint)(CLAMP(b, 0.0, 1.0) * 255.0 + 0.5);

	src        = cairo_image_surface_get_data(surface);
	dst        = cairo_image_surface_get_data(out);
	src_stride = cairo_image_surface_get_stride(surface);
	dst_stride = cairo_image_surface_get_stride(out);

	for (y = 0; y < h; y++) {
		const guint32 *srow = (const guint32 *)(src + (gsize)y * src_stride);
		guint32 *drow = (guint32 *)(dst + (gsize)y * dst_stride);

		for (x = 0; x < w; x++) {
			guint32 a = (srow[x] >> 24) & 0xff;

			/* Premultiplied, because that is what the format
			 * says and cairo does not check. */
			drow[x] = (a << 24)
			        | ((guint32)((ri * a + 127) / 255) << 16)
			        | ((guint32)((gi * a + 127) / 255) <<  8)
			        |  (guint32)((bi * a + 127) / 255);
		}
	}

	cairo_surface_mark_dirty(out);
	return out;
}

cairo_surface_t *
gowl_bar_icon_lookup(GHashTable *cache, const gchar *name,
                     const gchar *theme_path, gint size)
{
	g_autofree gchar *key = NULL;
	cairo_surface_t *surface = NULL;

	/* The cache OWNS the surface this returns, so there has to be one:
	 * without it the caller is handed a pointer nothing will ever
	 * free.  A one-off wants gowl_bar_icon_from_file() instead. */
	g_return_val_if_fail(cache != NULL, NULL);
	if (name == NULL || *name == '\0' || size <= 0)
		return NULL;

	key = g_strdup_printf("%s\x1f%s\x1f%d", name,
	                      theme_path != NULL ? theme_path : "", size);
	if (g_hash_table_lookup_extended(cache, key, NULL,
	                                 (gpointer *)&surface))
		return surface;

	{
		g_autofree gchar *path = find_icon_file(name, theme_path, size);

		surface = surface_from_file(path, size);
	}

	/* Inserted even when NULL: a name that resolves to nothing is
	 * searched for once, not on every repaint. */
	g_hash_table_insert(cache, g_steal_pointer(&key), surface);
	return surface;
}
