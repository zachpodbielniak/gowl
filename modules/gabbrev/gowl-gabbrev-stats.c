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
 * gowl-gabbrev-stats.c - Per-match usage counts.  See the header.
 */

#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "gowl-gabbrev"

#include <errno.h>
#include <glib/gstdio.h>
#include <stdlib.h>
#include <string.h>

#include "gowl-gabbrev-stats.h"

#define GABBREV_STATS_HEADER \
	"# gowl gabbrev usage: COUNT\tFIRST\tLAST\tID (Unix seconds; " \
	"the id C-escaped)\n"

struct _GabbrevStats {
	GHashTable *records;   /* gchar* id -> GabbrevStat* */
	gboolean    dirty;     /* changed since the last load or save */
};

/**
 * gabbrev_stats_new:
 *
 * Returns: (transfer full): an empty set of records
 */
GabbrevStats *
gabbrev_stats_new(void)
{
	GabbrevStats *stats;

	stats = g_new0(GabbrevStats, 1);
	stats->records = g_hash_table_new_full(g_str_hash, g_str_equal,
	                                       g_free, g_free);
	return stats;
}

/**
 * gabbrev_stats_free:
 * @stats: (nullable): the records
 */
void
gabbrev_stats_free(GabbrevStats *stats)
{
	if (stats == NULL)
		return;
	g_hash_table_unref(stats->records);
	g_free(stats);
}

/**
 * gabbrev_stats_record:
 * @stats: the records
 * @id: the match that expanded
 * @when: Unix seconds
 *
 * One more use of @id.  A clock that went backwards still counts the
 * use but never moves `last' back, so the newest use stays the newest.
 */
void
gabbrev_stats_record(
	GabbrevStats *stats,
	const gchar  *id,
	gint64        when
){
	GabbrevStat *s;

	g_return_if_fail(stats != NULL);
	if (id == NULL || *id == '\0')
		return;
	s = g_hash_table_lookup(stats->records, id);
	if (s == NULL) {
		s = g_new0(GabbrevStat, 1);
		s->first = when;
		g_hash_table_insert(stats->records, g_strdup(id), s);
	}
	s->count++;
	if (when > s->last)
		s->last = when;
	if (when < s->first)
		s->first = when;
	stats->dirty = TRUE;
}

/**
 * gabbrev_stats_lookup:
 * @stats: the records
 * @id: a match id
 *
 * Returns: (transfer none) (nullable): @id's record, %NULL if never used
 */
const GabbrevStat *
gabbrev_stats_lookup(
	GabbrevStats *stats,
	const gchar  *id
){
	g_return_val_if_fail(stats != NULL, NULL);
	return id != NULL ? g_hash_table_lookup(stats->records, id) : NULL;
}

/**
 * gabbrev_stats_forget:
 * @stats: the records
 * @id: a match id
 *
 * Returns: %TRUE if @id had a record
 */
gboolean
gabbrev_stats_forget(
	GabbrevStats *stats,
	const gchar  *id
){
	g_return_val_if_fail(stats != NULL, FALSE);
	if (id == NULL || !g_hash_table_remove(stats->records, id))
		return FALSE;
	stats->dirty = TRUE;
	return TRUE;
}

/**
 * gabbrev_stats_clear:
 * @stats: the records
 *
 * Forgets every record.
 */
void
gabbrev_stats_clear(GabbrevStats *stats)
{
	g_return_if_fail(stats != NULL);
	if (g_hash_table_size(stats->records) > 0)
		stats->dirty = TRUE;
	g_hash_table_remove_all(stats->records);
}

/**
 * gabbrev_stats_size:
 * @stats: the records
 *
 * Returns: how many matches have a record
 */
guint
gabbrev_stats_size(GabbrevStats *stats)
{
	g_return_val_if_fail(stats != NULL, 0);
	return g_hash_table_size(stats->records);
}

/* Most used first; between equals, most recently used first; then by
   id, so the order never depends on the hash table's. */
static gint
compare_ids(
	gconstpointer a,
	gconstpointer b,
	gpointer      data
){
	GHashTable *records = data;
	const gchar *ia = *(const gchar * const *)a;
	const gchar *ib = *(const gchar * const *)b;
	const GabbrevStat *sa = g_hash_table_lookup(records, ia);
	const GabbrevStat *sb = g_hash_table_lookup(records, ib);

	if (sa->count != sb->count)
		return sa->count > sb->count ? -1 : 1;
	if (sa->last != sb->last)
		return sa->last > sb->last ? -1 : 1;
	return strcmp(ia, ib);
}

/**
 * gabbrev_stats_ids:
 * @stats: the records
 *
 * Returns: (transfer full): every id with a record, most used first
 */
GStrv
gabbrev_stats_ids(GabbrevStats *stats)
{
	GPtrArray *out;
	GHashTableIter it;
	gpointer key;

	g_return_val_if_fail(stats != NULL, NULL);
	out = g_ptr_array_new();
	g_hash_table_iter_init(&it, stats->records);
	while (g_hash_table_iter_next(&it, &key, NULL))
		g_ptr_array_add(out, g_strdup(key));
	g_ptr_array_sort_with_data(out, compare_ids, stats->records);
	g_ptr_array_add(out, NULL);
	return (GStrv)g_ptr_array_free(out, FALSE);
}

/**
 * gabbrev_stats_is_dirty:
 * @stats: the records
 *
 * Returns: %TRUE when something changed since the last load or save
 */
gboolean
gabbrev_stats_is_dirty(GabbrevStats *stats)
{
	g_return_val_if_fail(stats != NULL, FALSE);
	return stats->dirty;
}

/* A decimal field, whole or not at all */
static gboolean
parse_int(
	const gchar *text,
	gint64      *out
){
	gchar *end = NULL;
	gint64 v;

	errno = 0;
	v = g_ascii_strtoll(text, &end, 10);
	if (errno != 0 || end == text || *end != '\0')
		return FALSE;
	*out = v;
	return TRUE;
}

/**
 * gabbrev_stats_load:
 * @stats: the records
 * @path: the file
 * @error: (nullable): where a read failure goes
 *
 * Merges the records in @path into @stats: counts add up, so a load
 * after uses in this session loses neither.  A missing file is not an
 * error -- nothing has been used yet.
 *
 * Returns: %FALSE if @path exists and cannot be read
 */
gboolean
gabbrev_stats_load(
	GabbrevStats *stats,
	const gchar  *path,
	GError      **error
){
	g_autofree gchar *text = NULL;
	g_auto(GStrv) lines = NULL;
	g_autoptr(GError) local = NULL;
	gboolean was_dirty;
	guint i;

	g_return_val_if_fail(stats != NULL, FALSE);
	g_return_val_if_fail(path != NULL, FALSE);

	if (!g_file_get_contents(path, &text, NULL, &local)) {
		if (g_error_matches(local, G_FILE_ERROR, G_FILE_ERROR_NOENT))
			return TRUE;
		g_propagate_error(error, g_steal_pointer(&local));
		return FALSE;
	}

	/* Merging makes the records differ from the file only when there
	   were records before; a load into nothing is clean */
	was_dirty = stats->dirty || g_hash_table_size(stats->records) > 0;
	lines = g_strsplit(text, "\n", -1);
	for (i = 0; lines[i] != NULL; i++) {
		g_auto(GStrv) f = NULL;
		g_autofree gchar *id = NULL;
		GabbrevStat *s;
		gint64 count, first, last;

		if (lines[i][0] == '#' || lines[i][0] == '\0')
			continue;
		f = g_strsplit(lines[i], "\t", 4);
		if (g_strv_length(f) != 4
		    || !parse_int(f[0], &count) || count <= 0
		    || !parse_int(f[1], &first) || !parse_int(f[2], &last))
			continue;
		id = g_strcompress(f[3]);
		if (*id == '\0' || !g_utf8_validate(id, -1, NULL))
			continue;
		s = g_hash_table_lookup(stats->records, id);
		if (s == NULL) {
			s = g_new0(GabbrevStat, 1);
			s->first = first;
			g_hash_table_insert(stats->records, g_steal_pointer(&id), s);
		}
		s->count += (guint64)count;
		if (first < s->first)
			s->first = first;
		if (last > s->last)
			s->last = last;
	}
	stats->dirty = was_dirty;
	return TRUE;
}

/**
 * gabbrev_stats_save:
 * @stats: the records
 * @path: the file; its directory is made if missing
 * @error: (nullable): where a write failure goes
 *
 * Writes every record, atomically (a crash leaves the old file or the
 * new one, never half of either), and clears the dirty flag.
 *
 * Returns: %FALSE on a write failure
 */
gboolean
gabbrev_stats_save(
	GabbrevStats *stats,
	const gchar  *path,
	GError      **error
){
	g_autoptr(GString) out = NULL;
	g_autofree gchar *dir = NULL;
	g_auto(GStrv) ids = NULL;
	guint i;

	g_return_val_if_fail(stats != NULL, FALSE);
	g_return_val_if_fail(path != NULL, FALSE);

	dir = g_path_get_dirname(path);
	if (g_mkdir_with_parents(dir, 0700) != 0) {
		gint e = errno;

		g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(e),
		            "cannot make %s: %s", dir, g_strerror(e));
		return FALSE;
	}

	out = g_string_new(GABBREV_STATS_HEADER);
	ids = gabbrev_stats_ids(stats);
	for (i = 0; ids[i] != NULL; i++) {
		const GabbrevStat *s = g_hash_table_lookup(stats->records, ids[i]);
		g_autofree gchar *esc = g_strescape(ids[i], NULL);

		g_string_append_printf(out,
			"%" G_GUINT64_FORMAT "\t%" G_GINT64_FORMAT "\t%"
			G_GINT64_FORMAT "\t%s\n",
			s->count, s->first, s->last, esc);
	}
	if (!g_file_set_contents_full(path, out->str, (gssize)out->len,
	                              G_FILE_SET_CONTENTS_CONSISTENT,
	                              0600, error))
		return FALSE;
	stats->dirty = FALSE;
	return TRUE;
}
