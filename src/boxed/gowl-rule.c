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

#include "gowl-rule.h"

G_DEFINE_BOXED_TYPE(GowlRule, gowl_rule,
                    gowl_rule_copy, gowl_rule_free)

/**
 * gowl_rule_new:
 * @app_id_pattern: (nullable): shell-style glob for app_id, or %NULL for any
 * @title_pattern: (nullable): shell-style glob for title, or %NULL for any
 * @tags: tag bitmask to assign (0 means leave unchanged)
 * @floating: %TRUE to float matching clients
 * @monitor: preferred monitor index, or -1 for no preference
 *
 * Allocates a new #GowlRule.  Pattern strings are copied; the caller keeps
 * ownership of the originals.
 *
 * Returns: (transfer full): a newly allocated #GowlRule. Free with
 *          gowl_rule_free().
 */
GowlRule *
gowl_rule_new(
	const gchar *app_id_pattern,
	const gchar *title_pattern,
	guint32      tags,
	gboolean     floating,
	gint         monitor
){
	GowlRule *self;

	self = g_slice_new(GowlRule);
	self->app_id_pattern = g_strdup(app_id_pattern);
	self->title_pattern = g_strdup(title_pattern);
	self->tags = tags;
	self->floating = floating;
	self->monitor = monitor;

	return self;
}

/**
 * gowl_rule_copy:
 * @self: (not nullable): a #GowlRule to copy
 *
 * Creates a deep copy of @self, duplicating all pattern strings.
 *
 * Returns: (transfer full): a newly allocated copy of @self. Free with
 *          gowl_rule_free().
 */
GowlRule *
gowl_rule_copy(const GowlRule *self)
{
	g_return_val_if_fail(self != NULL, NULL);

	return gowl_rule_new(self->app_id_pattern,
	                      self->title_pattern,
	                      self->tags,
	                      self->floating,
	                      self->monitor);
}

/**
 * gowl_rule_free:
 * @self: (nullable): a #GowlRule to free
 *
 * Releases all memory associated with @self, including pattern strings.
 * Safe to call with %NULL.
 */
void
gowl_rule_free(GowlRule *self)
{
	if (self != NULL) {
		g_free(self->app_id_pattern);
		g_free(self->title_pattern);
		g_slice_free(GowlRule, self);
	}
}

/**
 * gowl_rule_matches:
 * @self: (not nullable): a #GowlRule
 * @app_id: (nullable): the client's app_id string
 * @title: (nullable): the client's title string
 *
 * Tests whether a client with the given @app_id and @title matches this
 * rule.  A %NULL pattern acts as a wildcard (always matches).  Pattern
 * matching uses g_pattern_match_simple(), which provides shell-style
 * globbing (e.g. "firefox*").
 *
 * Returns: %TRUE if the client matches this rule, %FALSE otherwise.
 */
gboolean
gowl_rule_matches(
	const GowlRule *self,
	const gchar    *app_id,
	const gchar    *title
){
	gboolean app_id_ok;
	gboolean title_ok;

	g_return_val_if_fail(self != NULL, FALSE);

	/* NULL pattern = wildcard (matches everything) */
	if (self->app_id_pattern != NULL) {
		app_id_ok = (app_id != NULL &&
		             g_pattern_match_simple(self->app_id_pattern, app_id));
	} else {
		app_id_ok = TRUE;
	}

	if (self->title_pattern != NULL) {
		title_ok = (title != NULL &&
		            g_pattern_match_simple(self->title_pattern, title));
	} else {
		title_ok = TRUE;
	}

	return (app_id_ok && title_ok);
}
