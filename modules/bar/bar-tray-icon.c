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
 * The theme lookup itself is barkit's (barkit/gowl-bar-icon.h): it is
 * the icon theme specification and has nothing to do with trays, and
 * the menu's application rows want the same answer.  What is left here
 * is the part that IS about trays --- which of the three things an item
 * offers to believe, and in what order.
 */

#include "bar-tray-icon.h"

#include "barkit/gowl-bar-icon.h"

#include <string.h>


/* ── The cache ───────────────────────────────────────────────────── */

BarTrayIconCache *
bar_tray_icon_cache_new(void)
{
	BarTrayIconCache *cache = g_new0(BarTrayIconCache, 1);

	cache->items = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
		(GDestroyNotify)cairo_surface_destroy);
	cache->names = gowl_bar_icon_cache_new();
	return cache;
}

void
bar_tray_icon_cache_free(BarTrayIconCache *cache)
{
	if (cache == NULL)
		return;
	g_clear_pointer(&cache->items, g_hash_table_unref);
	g_clear_pointer(&cache->names, g_hash_table_unref);
	g_free(cache);
}

cairo_surface_t *
bar_tray_icon_for(BarTrayIconCache *cache, const GowlTrayItem *item,
                  gint size, const gdouble *fg)
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
	cache_key = g_strdup_printf("%s\x1f%u\x1f%d\x1f%02x%02x%02x",
	                            item->key, item->serial, size,
	                            fg != NULL ? (guint)(CLAMP(fg[0], 0.0, 1.0) * 255) : 0,
	                            fg != NULL ? (guint)(CLAMP(fg[1], 0.0, 1.0) * 255) : 0,
	                            fg != NULL ? (guint)(CLAMP(fg[2], 0.0, 1.0) * 255) : 0);
	if (g_hash_table_lookup_extended(cache->items, cache_key, NULL,
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
			surface = gowl_bar_icon_scale(raw, size);
			cairo_surface_destroy(raw);
		}
	}

	if (surface == NULL && name != NULL) {
		cairo_surface_t *found = gowl_bar_icon_lookup(
			cache->names, name, item->icon_theme_path, size);

		/* Borrowed from the shared cache, and this one is owned per
		 * item and per serial, so take a copy of the reference. */
		if (found != NULL)
			surface = cairo_surface_reference(found);
	}

	/* Last: the application's own Id as an icon name.  A guess, and a
	 * good one -- an application's tray icon is very often installed
	 * under the same name the application calls itself. */
	if (surface == NULL && item->id != NULL) {
		g_autofree gchar *lower = g_ascii_strdown(item->id, -1);
		cairo_surface_t *found = gowl_bar_icon_lookup(
			cache->names, lower, item->icon_theme_path, size);

		if (found != NULL)
			surface = cairo_surface_reference(found);
	}

	/*
	 * A symbolic icon is a shape in the alpha channel and a flat
	 * black in the colour one, and a flat black glyph on a dark bar
	 * is a slot that looks empty --- which reads as the application
	 * having failed to register rather than as an icon nobody can
	 * see.  Paint it in the bar's own foreground, the way the panel
	 * a symbolic icon was drawn for would have.
	 *
	 * Only a mask: an icon with any colour of its own is a picture
	 * and keeps it.  The lookup path hands back a surface the shared
	 * name cache owns, so the tint lands on a copy either way.
	 */
	if (surface != NULL && fg != NULL && gowl_bar_icon_is_mask(surface)) {
		cairo_surface_t *tinted = gowl_bar_icon_tint(surface, fg[0],
		                                             fg[1], fg[2]);

		if (tinted != NULL) {
			cairo_surface_destroy(surface);
			surface = tinted;
		}
	}

	/* Inserted even when NULL, so a name that cannot be resolved is
	 * searched for once rather than on every repaint. */
	g_hash_table_insert(cache->items, g_steal_pointer(&cache_key), surface);
	return surface;
}
