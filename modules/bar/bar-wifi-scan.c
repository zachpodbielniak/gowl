/*
 * gowl - GObject Wayland Compositor
 * Copyright (C) 2026  Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "bar-wifi-scan.h"

#include <string.h>

/* Strongest signal first.  The rows are "ssid\tsignal\tsecurity\tactive",
   so the number to compare is the second field. */
static gint
scan_cmp_signal(gconstpointer a, gconstpointer b)
{
	const gchar *ra = *(const gchar * const *)a;
	const gchar *rb = *(const gchar * const *)b;
	const gchar *pa = strchr(ra, '\t');
	const gchar *pb = strchr(rb, '\t');
	gint sa = pa != NULL ? (gint)g_ascii_strtoll(pa + 1, NULL, 10) : 0;
	gint sb = pb != NULL ? (gint)g_ascii_strtoll(pb + 1, NULL, 10) : 0;

	return sb - sa;
}

void
bar_wifi_scan_parse(GPtrArray *out, const gchar *nmcli_output, guint limit)
{
	g_auto(GStrv) lines = NULL;
	g_autoptr(GHashTable) best = NULL;
	g_autoptr(GHashTable) rows = NULL;
	GHashTableIter rit;
	gpointer rv;
	gint i;

	if (out == NULL)
		return;
	g_ptr_array_set_size(out, 0);
	if (nmcli_output == NULL)
		return;

	best = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	rows = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);

	lines = g_strsplit(nmcli_output, "\n", -1);
	for (i = 0; lines[i] != NULL; i++) {
		GString   *field;
		GPtrArray *fields;
		const gchar *p;

		if (lines[i][0] == '\0')
			continue;

		/* Terse output escapes a colon inside a field, and an SSID
		   may legitimately contain one. */
		fields = g_ptr_array_new_with_free_func(g_free);
		field  = g_string_new(NULL);
		for (p = lines[i]; *p != '\0'; p++) {
			if (*p == '\\' && p[1] != '\0') {
				p++;
				g_string_append_c(field, *p);
				continue;
			}
			if (*p == ':') {
				g_ptr_array_add(fields,
					g_string_free(field, FALSE));
				field = g_string_new(NULL);
				continue;
			}
			g_string_append_c(field, *p);
		}
		g_ptr_array_add(fields, g_string_free(field, FALSE));

		if (fields->len >= 4) {
			const gchar *ssid = g_ptr_array_index(fields, 1);

			if (ssid != NULL && ssid[0] != '\0') {
				const gchar *sig = g_ptr_array_index(fields, 2);
				const gchar *sec = g_ptr_array_index(fields, 3);
				const gchar *act = g_ptr_array_index(fields, 0);
				gint     strength;
				gpointer seen;

				strength = (gint)g_ascii_strtoll(sig, NULL, 10);

				/*
				 * One entry per NETWORK, not per radio.
				 *
				 * nmcli lists one row per BSS, so a router
				 * advertising 2.4 and 5 GHz shows the same
				 * SSID twice and a mesh shows it once per
				 * node.  That is what filled the panel with
				 * duplicates and pushed real networks off the
				 * end of it.  Keep the strongest sighting.
				 */
				seen = g_hash_table_lookup(best, ssid);
				if (seen == NULL
				    || strength > GPOINTER_TO_INT(seen)) {
					const gchar *prev;
					gboolean was_active = FALSE;

					/*
					 * "Connected" belongs to the NETWORK, not
					 * to the radio: on a mesh the BSS you are
					 * associated with is often not the
					 * strongest sighting of the same SSID, and
					 * keeping only the strongest row dropped
					 * the active flag with the weaker one --
					 * so the panel showed the network you were
					 * on as one you could connect to.
					 */
					prev = g_hash_table_lookup(rows, ssid);
					if (prev != NULL) {
						const gchar *tail;

						tail = strrchr(prev, '\t');
						was_active = (tail != NULL &&
							g_strcmp0(tail + 1, "yes") == 0);
					}
					g_hash_table_insert(best,
						g_strdup(ssid),
						GINT_TO_POINTER(strength));
					g_hash_table_insert(rows,
						g_strdup(ssid),
						g_strdup_printf(
							"%s\t%s\t%s\t%s",
							ssid, sig, sec,
							was_active ? "yes" : act));
				} else if (g_strcmp0(act, "yes") == 0) {
					/* A weaker sighting that is the one we are
					   on: mark the row we kept. */
					const gchar *prev;

					prev = g_hash_table_lookup(rows, ssid);
					if (prev != NULL) {
						g_auto(GStrv) f = NULL;

						f = g_strsplit(prev, "\t", 4);
						if (g_strv_length(f) == 4) {
							g_hash_table_insert(rows,
								g_strdup(ssid),
								g_strdup_printf(
									"%s\t%s\t%s\tyes",
									f[0], f[1], f[2]));
						}
					}
				}
			}
		}
		g_ptr_array_unref(fields);
	}

	/*
	 * Rank, THEN truncate.  The old code truncated at twenty while
	 * calling it "the strongest twenty", which it never was: it kept
	 * the first twenty rows nmcli happened to emit, duplicates and all.
	 */
	g_hash_table_iter_init(&rit, rows);
	while (g_hash_table_iter_next(&rit, NULL, &rv))
		g_ptr_array_add(out, g_strdup((const gchar *)rv));
	g_ptr_array_sort(out, scan_cmp_signal);
	if (limit > 0 && out->len > limit)
		g_ptr_array_set_size(out, limit);
}
