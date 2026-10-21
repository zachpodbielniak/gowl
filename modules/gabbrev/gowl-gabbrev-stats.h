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
 * gowl-gabbrev-stats.h - How often each match was used, and when.
 *
 * One record per match id: how many times it expanded, when first and
 * when last (Unix seconds).  The point is pruning -- a snippet file
 * grows for years and nobody remembers which half of it is dead -- so
 * the records outlive the session in a small tab-separated file under
 * $XDG_STATE_HOME.  The module saves it a few seconds after a use, not
 * on every keystroke.  Nothing here knows about keys or windows; the
 * tests drive it with ids and timestamps.
 *
 * The file, one record per line after a comment header:
 *
 *   COUNT <TAB> FIRST <TAB> LAST <TAB> ID
 *
 * with the id C-escaped (g_strescape), so a tab or newline in a trigger
 * cannot split a record.  A line that does not parse is skipped, never
 * fatal: losing one count is better than losing all of them.
 */

#ifndef GOWL_GABBREV_STATS_H
#define GOWL_GABBREV_STATS_H

#include <glib.h>

G_BEGIN_DECLS

/* One match's record. */
typedef struct {
	guint64  count;
	gint64   first;     /* Unix seconds of the first use */
	gint64   last;      /* Unix seconds of the latest use */
} GabbrevStat;

typedef struct _GabbrevStats GabbrevStats;

GabbrevStats      *gabbrev_stats_new      (void);
void               gabbrev_stats_free     (GabbrevStats  *stats);
void               gabbrev_stats_record   (GabbrevStats  *stats,
                                           const gchar   *id,
                                           gint64         when);
const GabbrevStat *gabbrev_stats_lookup   (GabbrevStats  *stats,
                                           const gchar   *id);
gboolean           gabbrev_stats_forget   (GabbrevStats  *stats,
                                           const gchar   *id);
void               gabbrev_stats_clear    (GabbrevStats  *stats);
guint              gabbrev_stats_size     (GabbrevStats  *stats);
GStrv              gabbrev_stats_ids      (GabbrevStats  *stats);
gboolean           gabbrev_stats_is_dirty (GabbrevStats  *stats);
gboolean           gabbrev_stats_load     (GabbrevStats  *stats,
                                           const gchar   *path,
                                           GError       **error);
gboolean           gabbrev_stats_save     (GabbrevStats  *stats,
                                           const gchar   *path,
                                           GError       **error);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(GabbrevStats, gabbrev_stats_free)

G_END_DECLS

#endif /* GOWL_GABBREV_STATS_H */
