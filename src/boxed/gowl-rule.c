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

/* Forward declarations for static helpers. */
static void gowl_rule_compile_regex(GowlRule *self);

/**
 * gowl_rule_new:
 * @app_id_pattern: (nullable): shell-style glob for app_id, or %NULL for any
 * @title_pattern: (nullable): shell-style glob for title, or %NULL for any
 * @tags: tag bitmask to assign (0 means leave unchanged)
 * @floating: %TRUE to float matching clients
 * @monitor: preferred monitor index, or -1 for no preference
 *
 * Allocates a new #GowlRule in glob mode with no geometry
 * override.  Pattern strings are copied; the caller keeps
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
	return gowl_rule_new_full(app_id_pattern, title_pattern,
	                           tags, floating, monitor,
	                           0, 0, FALSE, FALSE);
}

/**
 * gowl_rule_new_full:
 * @app_id_pattern: (nullable): pattern for app_id, or %NULL for any
 * @title_pattern: (nullable): pattern for title, or %NULL for any
 * @tags: tag bitmask to assign (0 means leave unchanged)
 * @floating: %TRUE to float matching clients
 * @monitor: preferred monitor index, or -1 for no preference
 * @width: explicit width in pixels, or 0 to keep natural size
 * @height: explicit height in pixels, or 0 to keep natural size
 * @center: center the client on its monitor when floating
 * @regex_mode: %TRUE to interpret the patterns as PCRE regexes
 *              (via #GRegex), %FALSE for shell glob matching
 *
 * Allocates a new #GowlRule exposing every tunable field.  When
 * @regex_mode is %TRUE the patterns are compiled immediately and
 * the compiled handles are cached on the rule.  A pattern that
 * fails to compile is kept as a literal string and a warning is
 * logged; the match function falls back to glob mode for that
 * pattern.
 *
 * Returns: (transfer full): a newly allocated #GowlRule. Free
 *          with gowl_rule_free().
 */
GowlRule *
gowl_rule_new_full(
	const gchar *app_id_pattern,
	const gchar *title_pattern,
	guint32      tags,
	gboolean     floating,
	gint         monitor,
	gint         width,
	gint         height,
	gboolean     center,
	gboolean     regex_mode
){
	GowlRule *self;

	self = g_slice_new0(GowlRule);
	self->app_id_pattern = g_strdup(app_id_pattern);
	self->title_pattern  = g_strdup(title_pattern);
	self->tags           = tags;
	self->floating       = floating;
	self->monitor        = monitor;
	self->width          = width;
	self->height         = height;
	self->center         = center;
	self->regex_mode     = regex_mode;
	self->app_id_regex   = NULL;
	self->title_regex    = NULL;

	if (regex_mode)
		gowl_rule_compile_regex(self);

	return self;
}

/**
 * gowl_rule_copy:
 * @self: (not nullable): a #GowlRule to copy
 *
 * Creates a deep copy of @self, duplicating all pattern strings.
 * The compiled regex handles (if any) are re-compiled on the copy
 * rather than shared, so each rule owns its own #GRegex.
 *
 * Returns: (transfer full): a newly allocated copy of @self. Free with
 *          gowl_rule_free().
 */
GowlRule *
gowl_rule_copy(const GowlRule *self)
{
	g_return_val_if_fail(self != NULL, NULL);

	return gowl_rule_new_full(self->app_id_pattern,
	                           self->title_pattern,
	                           self->tags,
	                           self->floating,
	                           self->monitor,
	                           self->width,
	                           self->height,
	                           self->center,
	                           self->regex_mode);
}

/**
 * gowl_rule_free:
 * @self: (nullable): a #GowlRule to free
 *
 * Releases all memory associated with @self, including pattern
 * strings and any compiled regex handles.  Safe to call with
 * %NULL.
 */
void
gowl_rule_free(GowlRule *self)
{
	if (self != NULL) {
		g_free(self->app_id_pattern);
		g_free(self->title_pattern);
		if (self->app_id_regex != NULL)
			g_regex_unref(self->app_id_regex);
		if (self->title_regex != NULL)
			g_regex_unref(self->title_regex);
		g_slice_free(GowlRule, self);
	}
}

/**
 * gowl_rule_compile_regex:
 * @self: a #GowlRule with regex_mode set
 *
 * Compiles the pattern strings into #GRegex handles when
 * @regex_mode is %TRUE.  A pattern that fails to compile is left
 * with a %NULL handle; the matcher falls back to glob matching in
 * that case.  Called automatically by gowl_rule_new_full().
 */
static void
gowl_rule_compile_regex(GowlRule *self)
{
	GError *err;

	if (self->app_id_pattern != NULL && self->app_id_regex == NULL) {
		err = NULL;
		self->app_id_regex = g_regex_new(self->app_id_pattern,
		                                  G_REGEX_OPTIMIZE, 0, &err);
		if (self->app_id_regex == NULL) {
			g_warning("gowl-rule: app_id regex '%s' failed: %s",
			          self->app_id_pattern,
			          err != NULL ? err->message : "(unknown)");
			g_clear_error(&err);
		}
	}

	if (self->title_pattern != NULL && self->title_regex == NULL) {
		err = NULL;
		self->title_regex = g_regex_new(self->title_pattern,
		                                 G_REGEX_OPTIMIZE, 0, &err);
		if (self->title_regex == NULL) {
			g_warning("gowl-rule: title regex '%s' failed: %s",
			          self->title_pattern,
			          err != NULL ? err->message : "(unknown)");
			g_clear_error(&err);
		}
	}
}

/**
 * gowl_rule_match_one:
 * @pattern: (nullable): the pattern string
 * @regex: (nullable): a compiled regex, if @regex_mode is %TRUE
 * @regex_mode: whether the rule is in regex mode
 * @value: (nullable): the value to test, e.g. app_id or title
 *
 * Tests a single field against its pattern.  A %NULL @pattern
 * matches everything.  A %NULL @value with a non-NULL pattern
 * never matches.
 *
 * Returns: %TRUE if matched
 */
static gboolean
gowl_rule_match_one(
	const gchar *pattern,
	GRegex      *regex,
	gboolean     regex_mode,
	const gchar *value
){
	if (pattern == NULL)
		return TRUE;
	if (value == NULL)
		return FALSE;

	if (regex_mode && regex != NULL)
		return g_regex_match(regex, value, 0, NULL);

	return g_pattern_match_simple(pattern, value);
}

/**
 * gowl_rule_matches:
 * @self: (not nullable): a #GowlRule
 * @app_id: (nullable): the client's app_id string
 * @title: (nullable): the client's title string
 *
 * Tests whether a client with the given @app_id and @title matches
 * this rule.  A %NULL pattern in the rule acts as a wildcard.
 * Matching uses PCRE (#GRegex) when the rule is in regex mode and
 * a compiled handle is present, otherwise g_pattern_match_simple()
 * for shell-style globbing.
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

	app_id_ok = gowl_rule_match_one(self->app_id_pattern,
	                                 self->app_id_regex,
	                                 self->regex_mode,
	                                 app_id);
	if (!app_id_ok)
		return FALSE;

	title_ok = gowl_rule_match_one(self->title_pattern,
	                                self->title_regex,
	                                self->regex_mode,
	                                title);

	return title_ok;
}
