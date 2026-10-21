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
 * gowl-gabbrev-model.c - Reading gabbrev and espanso configurations.
 *
 * The loader is told about SOURCES -- the built-in matches, files,
 * directories, espanso configuration directories, YAML nodes and text
 * -- in order, and builds one GabbrevSet from them.  Order matters in
 * two places: options set by a later source override an earlier one,
 * and when two matches have the same trigger the one loaded later wins
 * (so a user's `;today' replaces the built-in one).
 *
 * Every problem -- an unknown key, a regex that does not compile, a
 * file that is not there -- is recorded as one line and the rest still
 * loads: one typo must not cost every other snippet.  `gabbrev-status'
 * shows the lines.
 *
 * An espanso configuration directory is read the way espanso reads
 * it.  config/default.yml holds the options and says which match files
 * are in force (`includes', default every file under `../match/' minus those starting
 * with `_'); every other config/NAME.yml is an app-specific profile with
 * `filter_class'/`filter_title'/`filter_exec' regexes, its own options
 * and its own file selection.  Which files a profile allows is
 * resolved here, transitively through `imports:', so the engine only
 * asks "is this match's file in the set".
 */

#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "gowl-gabbrev"

#include "gowl-gabbrev-model.h"

#include <json-glib/json-glib.h>
#include <stdarg.h>
#include <string.h>

/* ── Options ─────────────────────────────────────────────────────────── */

#define DEFAULT_SEPARATORS " ,.?!\r\n\t'\"\f()[]{}<>:;\xc2\xa0"

/**
 * gabbrev_options_init_defaults: (skip)
 * @opts: options to fill
 *
 * espanso's defaults, except where gowl knows better (see the comments).
 * Every bit of `set' is on: these are a complete set.
 */
void
gabbrev_options_init_defaults(GabbrevOptions *opts)
{
	memset(opts, 0, sizeof *opts);
	opts->set = (guint)~0u;
	opts->enable = TRUE;
	opts->backend = GABBREV_BACKEND_AUTO;
	opts->clipboard_threshold = 100;
	opts->key_delay_ms = 0;
	opts->paste_shortcut = NULL;
	opts->preserve_clipboard = TRUE;
	opts->restore_delay_ms = 300;
	opts->pre_paste_delay_ms = 30;      /* our clipboard is set in-process */
	opts->post_paste_delay_ms = 80;
	opts->word_separators = g_strdup(DEFAULT_SEPARATORS);
	opts->undo_backspace = TRUE;
	opts->backspace_limit = 5;
	opts->regex_buffer = 30;
	opts->notifications = TRUE;
	opts->toggle_key = NULL;
	opts->search_shortcut = NULL;
	opts->search_trigger = NULL;
	opts->apply_patch = TRUE;
	opts->post_picker_delay_ms = 120;
	opts->refocus = TRUE;
	opts->timeout_ms = 120000;
	opts->auto_restart = TRUE;
}

/**
 * gabbrev_options_clear: (skip)
 * @opts: options
 */
void
gabbrev_options_clear(GabbrevOptions *opts)
{
	g_clear_pointer(&opts->paste_shortcut, g_free);
	g_clear_pointer(&opts->word_separators, g_free);
	g_clear_pointer(&opts->toggle_key, g_free);
	g_clear_pointer(&opts->search_shortcut, g_free);
	g_clear_pointer(&opts->search_trigger, g_free);
	opts->set = 0;
}

static void
copy_str(
	gchar       **dst,
	const gchar  *src
){
	g_free(*dst);
	*dst = g_strdup(src);
}

/**
 * gabbrev_options_merge: (skip)
 * @into: options to change
 * @over: options whose `set' bits win
 */
void
gabbrev_options_merge(
	GabbrevOptions       *into,
	const GabbrevOptions *over
){
	guint s = over->set;

	if (s & GABBREV_OPT_ENABLE)              into->enable = over->enable;
	if (s & GABBREV_OPT_BACKEND)             into->backend = over->backend;
	if (s & GABBREV_OPT_CLIPBOARD_THRESHOLD) into->clipboard_threshold = over->clipboard_threshold;
	if (s & GABBREV_OPT_KEY_DELAY)           into->key_delay_ms = over->key_delay_ms;
	if (s & GABBREV_OPT_PASTE_SHORTCUT)      copy_str(&into->paste_shortcut, over->paste_shortcut);
	if (s & GABBREV_OPT_PRESERVE_CLIPBOARD)  into->preserve_clipboard = over->preserve_clipboard;
	if (s & GABBREV_OPT_RESTORE_DELAY)       into->restore_delay_ms = over->restore_delay_ms;
	if (s & GABBREV_OPT_PRE_PASTE_DELAY)     into->pre_paste_delay_ms = over->pre_paste_delay_ms;
	if (s & GABBREV_OPT_POST_PASTE_DELAY)    into->post_paste_delay_ms = over->post_paste_delay_ms;
	if (s & GABBREV_OPT_WORD_SEPARATORS)     copy_str(&into->word_separators, over->word_separators);
	if (s & GABBREV_OPT_UNDO_BACKSPACE)      into->undo_backspace = over->undo_backspace;
	if (s & GABBREV_OPT_BACKSPACE_LIMIT)     into->backspace_limit = over->backspace_limit;
	if (s & GABBREV_OPT_REGEX_BUFFER)        into->regex_buffer = over->regex_buffer;
	if (s & GABBREV_OPT_NOTIFICATIONS)       into->notifications = over->notifications;
	if (s & GABBREV_OPT_TOGGLE_KEY)          copy_str(&into->toggle_key, over->toggle_key);
	if (s & GABBREV_OPT_SEARCH_SHORTCUT)     copy_str(&into->search_shortcut, over->search_shortcut);
	if (s & GABBREV_OPT_SEARCH_TRIGGER)      copy_str(&into->search_trigger, over->search_trigger);
	if (s & GABBREV_OPT_APPLY_PATCH)         into->apply_patch = over->apply_patch;
	if (s & GABBREV_OPT_POST_PICKER_DELAY)   into->post_picker_delay_ms = over->post_picker_delay_ms;
	if (s & GABBREV_OPT_REFOCUS)             into->refocus = over->refocus;
	if (s & GABBREV_OPT_TIMEOUT)             into->timeout_ms = over->timeout_ms;
	if (s & GABBREV_OPT_AUTO_RESTART)        into->auto_restart = over->auto_restart;
	into->set |= s;
}

/**
 * gabbrev_options_copy: (skip)
 * @dst: uninitialised options
 * @src: options to copy
 */
void
gabbrev_options_copy(
	GabbrevOptions       *dst,
	const GabbrevOptions *src
){
	memset(dst, 0, sizeof *dst);
	gabbrev_options_merge(dst, src);
	dst->set = src->set;
}

/* ── Variables and matches ───────────────────────────────────────────── */

/**
 * gabbrev_var_new: (skip)
 * @name: the variable's name
 * @type: its type
 * @params: (nullable) (transfer none): its params mapping
 *
 * Returns: (transfer full): a variable
 */
GabbrevVar *
gabbrev_var_new(
	const gchar *name,
	const gchar *type,
	YamlNode    *params
){
	GabbrevVar *v;

	v = g_new0(GabbrevVar, 1);
	v->name = g_strdup(name);
	v->type = g_strdup(type);
	v->params = params != NULL ? yaml_node_ref(params) : NULL;
	v->inject_vars = TRUE;
	return v;
}

/**
 * gabbrev_var_free: (skip)
 * @var: (transfer full): a variable
 */
void
gabbrev_var_free(GabbrevVar *var)
{
	if (var == NULL)
		return;
	g_free(var->name);
	g_free(var->type);
	if (var->params != NULL)
		yaml_node_unref(var->params);
	g_strfreev(var->depends_on);
	g_free(var);
}

/**
 * gabbrev_match_new: (skip)
 *
 * Returns: (transfer full): an empty match
 */
GabbrevMatch *
gabbrev_match_new(void)
{
	GabbrevMatch *m;

	m = g_new0(GabbrevMatch, 1);
	m->ref_count = 1;
	m->triggers = g_ptr_array_new_with_free_func(g_free);
	m->vars = g_ptr_array_new_with_free_func((GDestroyNotify)gabbrev_var_free);
	m->force_mode = -1;
	return m;
}

/**
 * gabbrev_match_ref: (skip)
 * @match: a match
 *
 * Returns: (transfer full): @match
 */
GabbrevMatch *
gabbrev_match_ref(GabbrevMatch *match)
{
	g_atomic_int_inc(&match->ref_count);
	return match;
}

/**
 * gabbrev_match_unref: (skip)
 * @match: (transfer full): a match
 */
void
gabbrev_match_unref(GabbrevMatch *match)
{
	if (match == NULL || !g_atomic_int_dec_and_test(&match->ref_count))
		return;
	g_free(match->id);
	g_ptr_array_unref(match->triggers);
	g_free(match->regex_text);
	if (match->regex != NULL)
		g_regex_unref(match->regex);
	g_free(match->replace);
	g_free(match->image_path);
	g_free(match->html);
	g_ptr_array_unref(match->vars);
	g_free(match->paste_shortcut);
	g_free(match->label);
	g_strfreev(match->search_terms);
	gowl_filter_free(match->filter);
	g_free(match->filter_text);
	if (match->filter_title != NULL)
		g_regex_unref(match->filter_title);
	if (match->filter_class != NULL)
		g_regex_unref(match->filter_class);
	if (match->filter_exec != NULL)
		g_regex_unref(match->filter_exec);
	g_free(match->source);
	g_free(match);
}

/**
 * gabbrev_match_is_static: (skip)
 * @match: a match
 *
 * Returns: %TRUE when the match is plain text: no variables and no
 *   `{{...}}' that could name a global one
 */
gboolean
gabbrev_match_is_static(const GabbrevMatch *match)
{
	return match->vars->len == 0 && match->replace != NULL
	       && strstr(match->replace, "{{") == NULL;
}

/**
 * gabbrev_profile_free: (skip)
 * @profile: (transfer full): a profile
 */
void
gabbrev_profile_free(GabbrevProfile *profile)
{
	if (profile == NULL)
		return;
	g_free(profile->name);
	if (profile->filter_title != NULL)
		g_regex_unref(profile->filter_title);
	if (profile->filter_class != NULL)
		g_regex_unref(profile->filter_class);
	if (profile->filter_exec != NULL)
		g_regex_unref(profile->filter_exec);
	g_free(profile->filter_os);
	gowl_filter_free(profile->filter);
	g_free(profile->filter_text);
	gabbrev_options_clear(&profile->options);
	if (profile->files != NULL)
		g_hash_table_unref(profile->files);
	g_free(profile);
}

/* ── The set ─────────────────────────────────────────────────────────── */

static GabbrevSet *
set_new(void)
{
	GabbrevSet *s;

	s = g_new0(GabbrevSet, 1);
	s->ref_count = 1;
	s->matches = g_ptr_array_new_with_free_func(
		(GDestroyNotify)gabbrev_match_unref);
	s->global_vars = g_ptr_array_new_with_free_func(
		(GDestroyNotify)gabbrev_var_free);
	gabbrev_options_init_defaults(&s->options);
	s->profiles = g_ptr_array_new_with_free_func(
		(GDestroyNotify)gabbrev_profile_free);
	s->problems = g_ptr_array_new_with_free_func(g_free);
	s->files = g_ptr_array_new_with_free_func(g_free);
	return s;
}

/**
 * gabbrev_set_ref: (skip)
 * @set: a set
 *
 * Returns: (transfer full): @set
 */
GabbrevSet *
gabbrev_set_ref(GabbrevSet *set)
{
	g_atomic_int_inc(&set->ref_count);
	return set;
}

/**
 * gabbrev_set_unref: (skip)
 * @set: (transfer full): a set
 */
void
gabbrev_set_unref(GabbrevSet *set)
{
	if (set == NULL || !g_atomic_int_dec_and_test(&set->ref_count))
		return;
	g_ptr_array_unref(set->matches);
	g_ptr_array_unref(set->global_vars);
	gabbrev_options_clear(&set->options);
	g_ptr_array_unref(set->profiles);
	if (set->default_files != NULL)
		g_hash_table_unref(set->default_files);
	g_ptr_array_unref(set->problems);
	g_ptr_array_unref(set->files);
	g_free(set);
}

/**
 * gabbrev_set_find_trigger: (skip)
 * @set: a set
 * @trigger: a trigger, exactly
 *
 * Returns: (transfer none) (nullable): the match the trigger types, the
 *   last loaded when several do
 */
GabbrevMatch *
gabbrev_set_find_trigger(
	GabbrevSet  *set,
	const gchar *trigger
){
	gint i;

	for (i = (gint)set->matches->len - 1; i >= 0; i--) {
		GabbrevMatch *m = g_ptr_array_index(set->matches, i);
		guint j;

		for (j = 0; j < m->triggers->len; j++)
			if (g_strcmp0(g_ptr_array_index(m->triggers, j), trigger) == 0)
				return m;
	}
	return NULL;
}

/**
 * gabbrev_set_find_id: (skip)
 * @set: a set
 * @id: a match id (its label, first trigger or regex) or any trigger
 *
 * Returns: (transfer none) (nullable): the match
 */
GabbrevMatch *
gabbrev_set_find_id(
	GabbrevSet  *set,
	const gchar *id
){
	gint i;

	for (i = (gint)set->matches->len - 1; i >= 0; i--) {
		GabbrevMatch *m = g_ptr_array_index(set->matches, i);

		if (g_strcmp0(m->id, id) == 0)
			return m;
	}
	return gabbrev_set_find_trigger(set, id);
}

/**
 * gabbrev_set_find_global: (skip)
 * @set: a set
 * @name: a global variable's name
 *
 * Returns: (transfer none) (nullable): the variable; a later definition
 *   of the same name wins
 */
const GabbrevVar *
gabbrev_set_find_global(
	GabbrevSet  *set,
	const gchar *name
){
	gint i;

	for (i = (gint)set->global_vars->len - 1; i >= 0; i--) {
		GabbrevVar *v = g_ptr_array_index(set->global_vars, i);

		if (g_strcmp0(v->name, name) == 0)
			return v;
	}
	return NULL;
}

/**
 * gabbrev_regexes_match: (skip)
 * @title: (nullable): matches the window title
 * @klass: (nullable): matches the app-id (X11: the class)
 * @exec: (nullable): matches the program's path
 * @window: the window
 *
 * espanso's rule: every filter given must match; none given matches.
 *
 * Returns: %TRUE when @window passes
 */
gboolean
gabbrev_regexes_match(
	GRegex              *title,
	GRegex              *klass,
	GRegex              *exec,
	const GabbrevWindow *window
){
	if (title != NULL && !g_regex_match(title, window->title != NULL
	                                    ? window->title : "", 0, NULL))
		return FALSE;
	if (klass != NULL && !g_regex_match(klass, window->app_id != NULL
	                                    ? window->app_id : "", 0, NULL))
		return FALSE;
	if (exec != NULL && !g_regex_match(exec, window->exec != NULL
	                                   ? window->exec : "", 0, NULL))
		return FALSE;
	return TRUE;
}

/**
 * gabbrev_set_profile_for: (skip)
 * @set: a set
 * @window: the window typed into
 *
 * Returns: (transfer none) (nullable): the first profile whose filters
 *   all match @window; %NULL means the default profile
 */
const GabbrevProfile *
gabbrev_set_profile_for(
	GabbrevSet          *set,
	const GabbrevWindow *window
){
	guint i;

	for (i = 0; i < set->profiles->len; i++) {
		GabbrevProfile *p = g_ptr_array_index(set->profiles, i);

		if (p->filter_os != NULL
		    && g_ascii_strcasecmp(p->filter_os, "linux") != 0)
			continue;
		if (p->filter_title == NULL && p->filter_class == NULL
		    && p->filter_exec == NULL && p->filter == NULL)
			continue;    /* a profile with no filter applies nowhere */
		if (!gabbrev_regexes_match(p->filter_title, p->filter_class,
		                           p->filter_exec, window))
			continue;
		if (p->filter != NULL
		    && (window->fields == NULL
		        || !gowl_filter_eval(p->filter, window->fields)))
			continue;
		return p;
	}
	return NULL;
}

/**
 * gabbrev_set_match_allowed: (skip)
 * @set: a set
 * @profile: (nullable): the window's profile; %NULL for the default
 * @match: a match of @set
 *
 * Whether @match is in force under @profile.  Only matches from files
 * are ever left out, by an espanso file selection; built-in, config,
 * code and runtime matches are everywhere a filter of their own allows.
 *
 * Returns: %TRUE when @match may expand
 */
gboolean
gabbrev_set_match_allowed(
	GabbrevSet           *set,
	const GabbrevProfile *profile,
	const GabbrevMatch   *match
){
	GHashTable *files;

	if (match->source == NULL || match->source[0] == '<')
		return TRUE;
	files = profile != NULL && profile->files != NULL ? profile->files
	        : set->default_files;
	return files == NULL || g_hash_table_contains(files, match->source);
}

/* ── Small helpers ───────────────────────────────────────────────────── */

/**
 * gabbrev_filter_fields: (skip)
 *
 * Returns: (transfer none): every field a gabbrev `filter:' may name
 */
const gchar * const *
gabbrev_filter_fields(void)
{
	static const gchar * const fields[] = {
		"app-id", "class", "title", "exec", "pid", "xwayland",
		"floating", "fullscreen", "monitor", "layout", "tag", "tags",
		"time", "hour", "weekday", "trigger",
		NULL
	};
	return fields;
}

/**
 * gabbrev_expand_path: (skip)
 * @path: a path, `~/' allowed
 * @base_dir: (nullable): what a relative path is relative to
 *
 * Returns: (transfer full): the absolute, canonical path
 */
gchar *
gabbrev_expand_path(
	const gchar *path,
	const gchar *base_dir
){
	g_autofree gchar *abs = NULL;

	if (g_str_has_prefix(path, "~/") || g_strcmp0(path, "~") == 0)
		abs = g_build_filename(g_get_home_dir(), path + 1, NULL);
	else if (g_str_has_prefix(path, "$XDG_CONFIG_HOME/"))
		abs = g_build_filename(g_get_user_config_dir(),
		                       path + strlen("$XDG_CONFIG_HOME/"), NULL);
	else if (g_path_is_absolute(path))
		abs = g_strdup(path);
	else
		abs = g_build_filename(base_dir != NULL ? base_dir
		                       : g_get_current_dir(), path, NULL);
	return g_canonicalize_filename(abs, NULL);
}

/* One glob segment character class `[...]' / `[!...]' at *p. */
static gboolean
class_match(
	const gchar **pp,
	gchar         c
){
	const gchar *p = *pp + 1;
	gboolean negate = FALSE;
	gboolean hit = FALSE;

	if (*p == '!' || *p == '^') {
		negate = TRUE;
		p++;
	}
	while (*p != '\0' && *p != ']') {
		if (p[1] == '-' && p[2] != '\0' && p[2] != ']') {
			if (c >= p[0] && c <= p[2])
				hit = TRUE;
			p += 3;
		} else {
			if (c == *p)
				hit = TRUE;
			p++;
		}
	}
	*pp = *p == ']' ? p + 1 : p;
	return negate ? !hit : hit;
}

/**
 * gabbrev_glob_match: (skip)
 * @pattern: a glob: `**' crosses directories, `*' and `?' do not,
 *   `[...]' and `[!...]' are classes
 * @path: a relative path, `/'-separated
 *
 * espanso's `includes' globs: `**' then `/' then `[!_]', star, `.yml'.
 *
 * Returns: %TRUE when @path matches
 */
gboolean
gabbrev_glob_match(
	const gchar *pattern,
	const gchar *path
){
	while (*pattern != '\0') {
		if (pattern[0] == '*' && pattern[1] == '*') {
			const gchar *rest = pattern + 2;
			const gchar *s;

			/* `**' then `/' also matches zero directories */
			if (*rest == '/') {
				if (gabbrev_glob_match(rest + 1, path))
					return TRUE;
			}
			for (s = path; ; s++) {
				if (gabbrev_glob_match(rest, s))
					return TRUE;
				if (*s == '\0')
					return FALSE;
			}
		}
		if (*pattern == '*') {
			const gchar *s;

			for (s = path; ; s++) {
				if (gabbrev_glob_match(pattern + 1, s))
					return TRUE;
				if (*s == '\0' || *s == '/')
					return FALSE;
			}
		}
		if (*path == '\0')
			return FALSE;
		if (*pattern == '?') {
			if (*path == '/')
				return FALSE;
			pattern++;
			path++;
			continue;
		}
		if (*pattern == '[') {
			if (*path == '/' || !class_match(&pattern, *path))
				return FALSE;
			path++;
			continue;
		}
		if (*pattern != *path)
			return FALSE;
		pattern++;
		path++;
	}
	return *path == '\0';
}

/**
 * gabbrev_node_to_json: (skip)
 * @node: (nullable): a node
 *
 * Returns: (transfer full): @node as JSON text (`null' for %NULL)
 */
gchar *
gabbrev_node_to_json(YamlNode *node)
{
	g_autoptr(JsonNode) j = NULL;

	if (node == NULL)
		return g_strdup("null");
	j = yaml_node_to_json_node(node);
	if (j == NULL)
		return g_strdup("null");
	return json_to_string(j, FALSE);
}

/**
 * gabbrev_node_to_string: (skip)
 * @node: (nullable): a node
 *
 * A param as text: a scalar as written, a list one element per line
 * (scalars) or as JSON, a mapping as JSON, null as "".
 *
 * Returns: (transfer full): the text
 */
gchar *
gabbrev_node_to_string(YamlNode *node)
{
	const gchar *s;

	if (node == NULL)
		return g_strdup("");
	switch (yaml_node_get_node_type(node)) {
	case YAML_NODE_SCALAR:
		s = yaml_node_get_scalar(node);
		return g_strdup(s != NULL ? s : "");
	case YAML_NODE_SEQUENCE: {
		YamlSequence *seq = yaml_node_get_sequence(node);
		GString *out = g_string_new(NULL);
		guint i;
		guint n = seq != NULL ? yaml_sequence_get_length(seq) : 0;

		for (i = 0; i < n; i++) {
			YamlNode *e = yaml_sequence_get_element(seq, i);

			if (e == NULL || yaml_node_get_node_type(e) != YAML_NODE_SCALAR) {
				g_string_free(out, TRUE);
				return gabbrev_node_to_json(node);
			}
			if (i > 0)
				g_string_append_c(out, '\n');
			g_string_append(out, yaml_node_get_scalar(e) != NULL
			                ? yaml_node_get_scalar(e) : "");
		}
		return g_string_free(out, FALSE);
	}
	case YAML_NODE_MAPPING:
		return gabbrev_node_to_json(node);
	case YAML_NODE_NULL:
	default:
		return g_strdup("");
	}
}

/* ── The loader ──────────────────────────────────────────────────────── */

struct _GabbrevLoader {
	GabbrevSet *set;
	GHashTable *loading;       /* files on the import stack (loop check) */
	GHashTable *loaded;        /* file -> GPtrArray* of every file it loaded */
	GHashTable *always_files;  /* files loaded outside an espanso selection */
	GHashTable *default_files; /* an espanso default selection, or NULL */
	guint       order;
	guint       depth;
	gboolean    in_selection;  /* loading for an espanso file selection */
};

/**
 * gabbrev_loader_new: (skip)
 *
 * Returns: (transfer full): a loader with nothing in it
 */
GabbrevLoader *
gabbrev_loader_new(void)
{
	GabbrevLoader *l;

	l = g_new0(GabbrevLoader, 1);
	l->set = set_new();
	l->loading = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	l->loaded = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
	                                  (GDestroyNotify)g_ptr_array_unref);
	l->always_files = g_hash_table_new_full(g_str_hash, g_str_equal,
	                                        g_free, NULL);
	return l;
}

/**
 * gabbrev_loader_free: (skip)
 * @loader: (transfer full): a loader
 */
void
gabbrev_loader_free(GabbrevLoader *loader)
{
	if (loader == NULL)
		return;
	if (loader->set != NULL)
		gabbrev_set_unref(loader->set);
	g_hash_table_unref(loader->loading);
	g_hash_table_unref(loader->loaded);
	g_hash_table_unref(loader->always_files);
	if (loader->default_files != NULL)
		g_hash_table_unref(loader->default_files);
	g_free(loader);
}

/**
 * gabbrev_loader_problem: (skip)
 * @loader: a loader
 * @format: printf format
 * @...: arguments
 *
 * Records one problem.  Logged at debug level only: the module reports
 * the count (and the first) once per load, and gabbrev-status lists all.
 */
void
gabbrev_loader_problem(
	GabbrevLoader *loader,
	const gchar   *format,
	...
){
	va_list ap;
	gchar *msg;

	va_start(ap, format);
	msg = g_strdup_vprintf(format, ap);
	va_end(ap);
	g_strdelimit(msg, "\n", ' ');
	g_debug("gabbrev: %s", msg);
	g_ptr_array_add(loader->set->problems, msg);
}

/* A key with `-' spelt as `_', so `left-word' and `left_word' agree. */
static gchar *
norm_key(const gchar *key)
{
	return g_strdelimit(g_strdup(key), "-", '_');
}

static const gchar *
scalar(YamlNode *node)
{
	if (node == NULL || yaml_node_get_node_type(node) != YAML_NODE_SCALAR)
		return NULL;
	return yaml_node_get_scalar(node);
}

static gboolean
parse_bool_text(
	const gchar *s,
	gboolean    *out
){
	if (s == NULL)
		return FALSE;
	if (g_ascii_strcasecmp(s, "true") == 0 || g_ascii_strcasecmp(s, "yes") == 0
	    || g_ascii_strcasecmp(s, "on") == 0 || g_strcmp0(s, "1") == 0) {
		*out = TRUE;
		return TRUE;
	}
	if (g_ascii_strcasecmp(s, "false") == 0 || g_ascii_strcasecmp(s, "no") == 0
	    || g_ascii_strcasecmp(s, "off") == 0 || g_strcmp0(s, "0") == 0) {
		*out = FALSE;
		return TRUE;
	}
	return FALSE;
}

static gboolean
get_bool(
	GabbrevLoader *l,
	const gchar   *where,
	const gchar   *key,
	YamlNode      *node,
	gboolean      *out
){
	if (parse_bool_text(scalar(node), out))
		return TRUE;
	gabbrev_loader_problem(l, "%s: `%s' wants true or false", where, key);
	return FALSE;
}

static gboolean
get_uint(
	GabbrevLoader *l,
	const gchar   *where,
	const gchar   *key,
	YamlNode      *node,
	guint          max,
	guint         *out
){
	guint64 n;

	if (scalar(node) != NULL
	    && g_ascii_string_to_unsigned(scalar(node), 10, 0, max, &n, NULL)) {
		*out = (guint)n;
		return TRUE;
	}
	gabbrev_loader_problem(l, "%s: `%s' wants a number 0..%u", where, key, max);
	return FALSE;
}

/* A list of strings; a lone scalar counts as a list of one. */
static GPtrArray *
get_strings(YamlNode *node)
{
	GPtrArray *out = g_ptr_array_new_with_free_func(g_free);

	if (node == NULL)
		return out;
	if (yaml_node_get_node_type(node) == YAML_NODE_SCALAR) {
		g_ptr_array_add(out, g_strdup(scalar(node)));
	} else if (yaml_node_get_node_type(node) == YAML_NODE_SEQUENCE) {
		YamlSequence *seq = yaml_node_get_sequence(node);
		guint i;

		for (i = 0; seq != NULL && i < yaml_sequence_get_length(seq); i++) {
			const gchar *s = scalar(yaml_sequence_get_element(seq, i));

			if (s != NULL)
				g_ptr_array_add(out, g_strdup(s));
		}
	}
	return out;
}

static GRegex *
get_regex(
	GabbrevLoader *l,
	const gchar   *where,
	const gchar   *key,
	YamlNode      *node
){
	g_autoptr(GError) error = NULL;
	const gchar *s = scalar(node);
	GRegex *re;

	if (s == NULL) {
		gabbrev_loader_problem(l, "%s: `%s' wants a regex", where, key);
		return NULL;
	}
	re = g_regex_new(s, G_REGEX_OPTIMIZE, 0, &error);
	if (re == NULL)
		gabbrev_loader_problem(l, "%s: `%s' %s", where, key, error->message);
	return re;
}

static GowlFilter *
get_filter(
	GabbrevLoader *l,
	const gchar   *where,
	YamlNode      *node,
	gchar        **out_text
){
	g_autoptr(GError) error = NULL;
	const gchar *s = scalar(node);
	GowlFilter *f;

	if (s == NULL) {
		gabbrev_loader_problem(l, "%s: `filter' wants a filter expression",
		                       where);
		return NULL;
	}
	f = gowl_filter_parse(s, gabbrev_filter_fields(), &error);
	if (f == NULL) {
		gabbrev_loader_problem(l, "%s: filter: %s", where, error->message);
		return NULL;
	}
	*out_text = g_strdup(s);
	return f;
}

/*
 * One option key into @opts, or FALSE when @key is not an option.
 * espanso's names, plus a handful of gowl's.  Options espanso has for
 * other platforms are accepted and ignored, so a config that also runs
 * under espanso loads without a complaint.
 */
static gboolean
parse_option(
	GabbrevLoader  *l,
	const gchar    *where,
	const gchar    *key,
	YamlNode       *val,
	GabbrevOptions *opts
){
	static const gchar * const ignored[] = {
		"show_icon", "keyboard_layout",
		"evdev_modifier_delay", "disable_x11_fast_inject",
		"x11_use_xclip_backend", "x11_use_xdotool_backend",
		"win32_exclude_orphan_events",
		"win32_keyboard_layout_cache_interval", "emulate_alt_codes",
		"max_form_height", "max_form_width", "inject_delay_auto",
		"word_separators_auto", "use_standard_includes",
		NULL
	};
	gboolean b;
	guint n;
	guint i;

	if (g_strcmp0(key, "enable") == 0) {
		if (get_bool(l, where, key, val, &b)) {
			opts->enable = b;
			opts->set |= GABBREV_OPT_ENABLE;
		}
		return TRUE;
	}
	if (g_strcmp0(key, "backend") == 0) {
		const gchar *s = scalar(val);

		if (s != NULL && g_ascii_strcasecmp(s, "auto") == 0)
			opts->backend = GABBREV_BACKEND_AUTO;
		else if (s != NULL && (g_ascii_strcasecmp(s, "inject") == 0
		                       || g_ascii_strcasecmp(s, "keys") == 0))
			opts->backend = GABBREV_BACKEND_KEYS;
		else if (s != NULL && g_ascii_strcasecmp(s, "clipboard") == 0)
			opts->backend = GABBREV_BACKEND_CLIPBOARD;
		else if (s != NULL && (g_ascii_strcasecmp(s, "text-input") == 0
		                       || g_ascii_strcasecmp(s, "text_input") == 0))
			opts->backend = GABBREV_BACKEND_TEXT_INPUT;
		else {
			gabbrev_loader_problem(l, "%s: backend is auto, inject, "
			                       "clipboard or text-input", where);
			return TRUE;
		}
		opts->set |= GABBREV_OPT_BACKEND;
		return TRUE;
	}
	if (g_strcmp0(key, "clipboard_threshold") == 0) {
		if (get_uint(l, where, key, val, 1000000, &n)) {
			opts->clipboard_threshold = n;
			opts->set |= GABBREV_OPT_CLIPBOARD_THRESHOLD;
		}
		return TRUE;
	}
	if (g_strcmp0(key, "key_delay") == 0 || g_strcmp0(key, "inject_delay") == 0) {
		if (get_uint(l, where, key, val, 1000, &n)) {
			opts->key_delay_ms = n;
			opts->set |= GABBREV_OPT_KEY_DELAY;
		}
		return TRUE;
	}
	if (g_strcmp0(key, "paste_shortcut") == 0) {
		copy_str(&opts->paste_shortcut, scalar(val));
		opts->set |= GABBREV_OPT_PASTE_SHORTCUT;
		return TRUE;
	}
	if (g_strcmp0(key, "preserve_clipboard") == 0) {
		if (get_bool(l, where, key, val, &b)) {
			opts->preserve_clipboard = b;
			opts->set |= GABBREV_OPT_PRESERVE_CLIPBOARD;
		}
		return TRUE;
	}
	if (g_strcmp0(key, "restore_clipboard_delay") == 0) {
		if (get_uint(l, where, key, val, 60000, &n)) {
			opts->restore_delay_ms = n;
			opts->set |= GABBREV_OPT_RESTORE_DELAY;
		}
		return TRUE;
	}
	if (g_strcmp0(key, "pre_paste_delay") == 0) {
		if (get_uint(l, where, key, val, 10000, &n)) {
			opts->pre_paste_delay_ms = n;
			opts->set |= GABBREV_OPT_PRE_PASTE_DELAY;
		}
		return TRUE;
	}
	if (g_strcmp0(key, "paste_shortcut_event_delay") == 0
	    || g_strcmp0(key, "post_paste_delay") == 0) {
		if (get_uint(l, where, key, val, 10000, &n)) {
			opts->post_paste_delay_ms = n;
			opts->set |= GABBREV_OPT_POST_PASTE_DELAY;
		}
		return TRUE;
	}
	if (g_strcmp0(key, "word_separators") == 0) {
		g_autoptr(GPtrArray) seps = get_strings(val);
		GString *s = g_string_new(NULL);

		for (i = 0; i < seps->len; i++)
			g_string_append(s, g_ptr_array_index(seps, i));
		g_free(opts->word_separators);
		opts->word_separators = g_string_free(s, FALSE);
		opts->set |= GABBREV_OPT_WORD_SEPARATORS;
		return TRUE;
	}
	if (g_strcmp0(key, "undo_backspace") == 0) {
		if (get_bool(l, where, key, val, &b)) {
			opts->undo_backspace = b;
			opts->set |= GABBREV_OPT_UNDO_BACKSPACE;
		}
		return TRUE;
	}
	if (g_strcmp0(key, "backspace_limit") == 0) {
		if (get_uint(l, where, key, val, 1000, &n)) {
			opts->backspace_limit = n;
			opts->set |= GABBREV_OPT_BACKSPACE_LIMIT;
		}
		return TRUE;
	}
	if (g_strcmp0(key, "max_regex_buffer_size") == 0) {
		if (get_uint(l, where, key, val, 4096, &n)) {
			opts->regex_buffer = n;
			opts->set |= GABBREV_OPT_REGEX_BUFFER;
		}
		return TRUE;
	}
	if (g_strcmp0(key, "show_notifications") == 0) {
		if (get_bool(l, where, key, val, &b)) {
			opts->notifications = b;
			opts->set |= GABBREV_OPT_NOTIFICATIONS;
		}
		return TRUE;
	}
	if (g_strcmp0(key, "toggle_key") == 0) {
		copy_str(&opts->toggle_key, scalar(val));
		opts->set |= GABBREV_OPT_TOGGLE_KEY;
		return TRUE;
	}
	if (g_strcmp0(key, "search_shortcut") == 0) {
		copy_str(&opts->search_shortcut, scalar(val));
		opts->set |= GABBREV_OPT_SEARCH_SHORTCUT;
		return TRUE;
	}
	if (g_strcmp0(key, "search_trigger") == 0) {
		const gchar *s = scalar(val);

		copy_str(&opts->search_trigger,
		         s != NULL && g_ascii_strcasecmp(s, "off") != 0 ? s : NULL);
		opts->set |= GABBREV_OPT_SEARCH_TRIGGER;
		return TRUE;
	}
	if (g_strcmp0(key, "apply_patch") == 0) {
		if (get_bool(l, where, key, val, &b)) {
			opts->apply_patch = b;
			opts->set |= GABBREV_OPT_APPLY_PATCH;
		}
		return TRUE;
	}
	if (g_strcmp0(key, "post_form_delay") == 0
	    || g_strcmp0(key, "post_search_delay") == 0
	    || g_strcmp0(key, "post_picker_delay") == 0) {
		if (get_uint(l, where, key, val, 10000, &n)) {
			opts->post_picker_delay_ms = n;
			opts->set |= GABBREV_OPT_POST_PICKER_DELAY;
		}
		return TRUE;
	}
	if (g_strcmp0(key, "refocus") == 0) {
		if (get_bool(l, where, key, val, &b)) {
			opts->refocus = b;
			opts->set |= GABBREV_OPT_REFOCUS;
		}
		return TRUE;
	}
	if (g_strcmp0(key, "timeout") == 0 || g_strcmp0(key, "timeout_ms") == 0) {
		if (get_uint(l, where, key, val, 3600000, &n)) {
			opts->timeout_ms = n;
			opts->set |= GABBREV_OPT_TIMEOUT;
		}
		return TRUE;
	}
	/* espanso restarts itself when a config file changes; gowl reloads
	   in place -- the same switch turns either off */
	if (g_strcmp0(key, "auto_restart") == 0 || g_strcmp0(key, "auto_reload") == 0) {
		if (get_bool(l, where, key, val, &b)) {
			opts->auto_restart = b;
			opts->set |= GABBREV_OPT_AUTO_RESTART;
		}
		return TRUE;
	}
	for (i = 0; ignored[i] != NULL; i++)
		if (g_strcmp0(key, ignored[i]) == 0)
			return TRUE;
	return FALSE;
}

/* ── Variables ──────────────────────────────────────────────────────── */

static GabbrevVar *
parse_var(
	GabbrevLoader *l,
	const gchar   *where,
	YamlNode      *node
){
	YamlMapping *m;
	const gchar *name = NULL;
	const gchar *type = NULL;
	YamlNode *params = NULL;
	GabbrevVar *v;
	gboolean inject = TRUE;
	g_autoptr(GPtrArray) deps = NULL;
	guint i;

	m = node != NULL ? yaml_node_get_mapping(node) : NULL;
	if (m == NULL) {
		gabbrev_loader_problem(l, "%s: a variable is a mapping with name "
		                       "and type", where);
		return NULL;
	}
	for (i = 0; i < yaml_mapping_get_size(m); i++) {
		g_autofree gchar *key = norm_key(yaml_mapping_get_key(m, i));
		YamlNode *val = yaml_mapping_get_value(m, i);

		if (g_strcmp0(key, "name") == 0)
			name = scalar(val);
		else if (g_strcmp0(key, "type") == 0)
			type = scalar(val);
		else if (g_strcmp0(key, "params") == 0)
			params = val;
		else if (g_strcmp0(key, "inject_vars") == 0)
			get_bool(l, where, key, val, &inject);
		else if (g_strcmp0(key, "depends_on") == 0)
			deps = get_strings(val);
		else
			gabbrev_loader_problem(l, "%s: variable: unknown key `%s'",
			                       where, key);
	}
	if (name == NULL || *name == '\0') {
		gabbrev_loader_problem(l, "%s: a variable has no name", where);
		return NULL;
	}
	if (type == NULL || *type == '\0') {
		gabbrev_loader_problem(l, "%s: variable `%s' has no type", where,
		                       name);
		return NULL;
	}
	if (params != NULL && yaml_node_get_node_type(params) != YAML_NODE_MAPPING
	    && yaml_node_get_node_type(params) != YAML_NODE_NULL) {
		gabbrev_loader_problem(l, "%s: variable `%s': params is a mapping",
		                       where, name);
		params = NULL;
	}
	if (params != NULL && yaml_node_get_node_type(params) == YAML_NODE_NULL)
		params = NULL;
	v = gabbrev_var_new(name, type, params);
	v->inject_vars = inject;
	if (deps != NULL) {
		g_ptr_array_add(deps, NULL);
		v->depends_on = (GStrv)g_ptr_array_free(g_steal_pointer(&deps), FALSE);
	}
	return v;
}

static void
parse_vars_into(
	GabbrevLoader *l,
	const gchar   *where,
	YamlNode      *node,
	GPtrArray     *into
){
	YamlSequence *seq;
	guint i;

	if (node == NULL || yaml_node_get_node_type(node) == YAML_NODE_NULL)
		return;
	seq = yaml_node_get_sequence(node);
	if (seq == NULL) {
		gabbrev_loader_problem(l, "%s: vars is a list", where);
		return;
	}
	for (i = 0; i < yaml_sequence_get_length(seq); i++) {
		GabbrevVar *v = parse_var(l, where, yaml_sequence_get_element(seq, i));

		if (v != NULL)
			g_ptr_array_add(into, v);
	}
}

/* ── Matches ─────────────────────────────────────────────────────────── */

/*
 * espanso's `form:' shorthand: `form: "Hi [[name]]"' with `form_fields'
 * becomes `replace: "Hi {{form1.name}}"' and one variable, form1, of
 * type form whose params are the layout and the fields.  The module (or
 * the embedder's `form' type) shows the form; its JSON answer fills
 * form1.name.
 */
static void
expand_form(
	GabbrevMatch *m,
	const gchar  *layout,
	YamlNode     *fields
){
	g_autoptr(GRegex) re = NULL;
	g_autoptr(YamlMapping) pm = NULL;
	g_autoptr(YamlNode) params = NULL;
	GabbrevVar *v;

	re = g_regex_new("\\[\\[\\s*([A-Za-z0-9_\\-]+)\\s*\\]\\]", 0, 0, NULL);
	g_free(m->replace);
	m->replace = g_regex_replace(re, layout, -1, 0, "{{form1.\\1}}", 0, NULL);

	pm = yaml_mapping_new();
	yaml_mapping_set_string_member(pm, "layout", layout);
	if (fields != NULL)
		yaml_mapping_set_member(pm, "fields", fields);
	params = yaml_node_new_mapping(pm);
	v = gabbrev_var_new("form1", "form", params);
	g_ptr_array_insert(m->vars, 0, v);
}

static GabbrevMatch *
parse_match(
	GabbrevLoader *l,
	const gchar   *where,
	YamlNode      *node,
	const gchar   *source
){
	GabbrevMatch *m;
	YamlMapping *map;
	YamlNode *form_fields = NULL;
	const gchar *form = NULL;
	const gchar *explicit_id = NULL;
	gboolean word = FALSE;
	gboolean b;
	guint i;

	map = node != NULL ? yaml_node_get_mapping(node) : NULL;
	if (map == NULL) {
		gabbrev_loader_problem(l, "%s: a match is a mapping", where);
		return NULL;
	}
	m = gabbrev_match_new();
	m->source = g_strdup(source);

	for (i = 0; i < yaml_mapping_get_size(map); i++) {
		g_autofree gchar *key = norm_key(yaml_mapping_get_key(map, i));
		YamlNode *val = yaml_mapping_get_value(map, i);

		if (g_strcmp0(key, "trigger") == 0) {
			if (scalar(val) != NULL && *scalar(val) != '\0')
				g_ptr_array_add(m->triggers, g_strdup(scalar(val)));
			else
				gabbrev_loader_problem(l, "%s: an empty trigger", where);
		} else if (g_strcmp0(key, "triggers") == 0) {
			g_autoptr(GPtrArray) ts = get_strings(val);
			guint j;

			for (j = 0; j < ts->len; j++)
				if (*(gchar *)g_ptr_array_index(ts, j) != '\0')
					g_ptr_array_add(m->triggers,
					                g_strdup(g_ptr_array_index(ts, j)));
		} else if (g_strcmp0(key, "regex") == 0) {
			g_autoptr(GError) error = NULL;
			g_autofree gchar *anchored = NULL;

			m->regex_text = g_strdup(scalar(val));
			if (m->regex_text == NULL) {
				gabbrev_loader_problem(l, "%s: regex wants text", where);
				continue;
			}
			/* Anchored at the cursor: the buffer's end */
			anchored = g_strdup_printf("(?:%s)\\z", m->regex_text);
			m->regex = g_regex_new(anchored, G_REGEX_OPTIMIZE, 0, &error);
			if (m->regex == NULL)
				gabbrev_loader_problem(l, "%s: regex: %s", where,
				                       error->message);
		} else if (g_strcmp0(key, "replace") == 0) {
			g_free(m->replace);
			m->replace = gabbrev_node_to_string(val);
		} else if (g_strcmp0(key, "markdown") == 0) {
			/* No renderer here: the text goes in as written */
			g_free(m->replace);
			m->replace = gabbrev_node_to_string(val);
			m->markdown = TRUE;
		} else if (g_strcmp0(key, "html") == 0) {
			g_free(m->html);
			m->html = gabbrev_node_to_string(val);
		} else if (g_strcmp0(key, "image_path") == 0) {
			g_free(m->image_path);
			m->image_path = scalar(val) != NULL
				? gabbrev_expand_path(scalar(val), NULL) : NULL;
		} else if (g_strcmp0(key, "form") == 0) {
			form = scalar(val);
		} else if (g_strcmp0(key, "form_fields") == 0) {
			form_fields = val;
		} else if (g_strcmp0(key, "vars") == 0) {
			parse_vars_into(l, where, val, m->vars);
		} else if (g_strcmp0(key, "word") == 0) {
			get_bool(l, where, key, val, &word);
		} else if (g_strcmp0(key, "left_word") == 0) {
			get_bool(l, where, key, val, &m->left_word);
		} else if (g_strcmp0(key, "right_word") == 0) {
			get_bool(l, where, key, val, &m->right_word);
		} else if (g_strcmp0(key, "propagate_case") == 0) {
			get_bool(l, where, key, val, &m->propagate_case);
		} else if (g_strcmp0(key, "uppercase_style") == 0) {
			const gchar *s = scalar(val);

			if (g_strcmp0(s, "uppercase") == 0)
				m->uppercase_style = GABBREV_CASE_UPPERCASE;
			else if (g_strcmp0(s, "capitalize") == 0)
				m->uppercase_style = GABBREV_CASE_CAPITALIZE;
			else if (g_strcmp0(s, "capitalize_words") == 0)
				m->uppercase_style = GABBREV_CASE_CAPITALIZE_WORDS;
			else
				gabbrev_loader_problem(l, "%s: uppercase_style is "
				                       "uppercase, capitalize or "
				                       "capitalize_words", where);
		} else if (g_strcmp0(key, "force_mode") == 0) {
			const gchar *s = scalar(val);

			if (g_strcmp0(s, "clipboard") == 0)
				m->force_mode = GABBREV_BACKEND_CLIPBOARD;
			else if (g_strcmp0(s, "keys") == 0 || g_strcmp0(s, "inject") == 0)
				m->force_mode = GABBREV_BACKEND_KEYS;
			else if (g_strcmp0(s, "text-input") == 0
			         || g_strcmp0(s, "text_input") == 0)
				m->force_mode = GABBREV_BACKEND_TEXT_INPUT;
			else
				gabbrev_loader_problem(l, "%s: force_mode is clipboard, "
				                       "keys or text-input", where);
		} else if (g_strcmp0(key, "force_clipboard") == 0) {
			if (get_bool(l, where, key, val, &b) && b)
				m->force_mode = GABBREV_BACKEND_CLIPBOARD;
		} else if (g_strcmp0(key, "paste_shortcut") == 0) {
			g_free(m->paste_shortcut);
			m->paste_shortcut = g_strdup(scalar(val));
		} else if (g_strcmp0(key, "label") == 0) {
			g_free(m->label);
			m->label = g_strdup(scalar(val));
		} else if (g_strcmp0(key, "id") == 0 || g_strcmp0(key, "name") == 0) {
			explicit_id = scalar(val);
		} else if (g_strcmp0(key, "search_terms") == 0) {
			g_autoptr(GPtrArray) ts = get_strings(val);

			g_ptr_array_add(ts, NULL);
			g_strfreev(m->search_terms);
			m->search_terms = (GStrv)g_ptr_array_free(
				g_steal_pointer(&ts), FALSE);
		} else if (g_strcmp0(key, "passive_only") == 0
		           || g_strcmp0(key, "search_only") == 0) {
			get_bool(l, where, key, val, &m->search_only);
		} else if (g_strcmp0(key, "filter") == 0) {
			gowl_filter_free(m->filter);
			g_clear_pointer(&m->filter_text, g_free);
			m->filter = get_filter(l, where, val, &m->filter_text);
		} else if (g_strcmp0(key, "filter_title") == 0) {
			m->filter_title = get_regex(l, where, key, val);
		} else if (g_strcmp0(key, "filter_class") == 0) {
			m->filter_class = get_regex(l, where, key, val);
		} else if (g_strcmp0(key, "filter_exec") == 0) {
			m->filter_exec = get_regex(l, where, key, val);
		} else if (g_strcmp0(key, "delete_first") == 0) {
			get_bool(l, where, key, val, &m->delete_first);
		} else if (g_strcmp0(key, "notify") == 0) {
			get_bool(l, where, key, val, &m->notify);
		} else if (g_strcmp0(key, "timeout") == 0
		           || g_strcmp0(key, "timeout_ms") == 0) {
			get_uint(l, where, key, val, 3600000, &m->timeout_ms);
		} else if (g_strcmp0(key, "paragraph") == 0
		           || g_strcmp0(key, "description") == 0
		           || g_str_has_prefix(key, "x_")) {
			/* accepted: paragraph only matters to a markdown
			   renderer; x- keys are the user's own notes */
		} else {
			gabbrev_loader_problem(l, "%s: unknown key `%s'", where, key);
		}
	}

	if (form != NULL)
		expand_form(m, form, form_fields);
	if (word)
		m->left_word = m->right_word = TRUE;

	if (m->triggers->len == 0 && m->regex == NULL) {
		if (m->regex_text == NULL)
			gabbrev_loader_problem(l, "%s: a match needs trigger, triggers "
			                       "or regex", where);
		gabbrev_match_unref(m);
		return NULL;
	}
	if (m->replace == NULL && m->html == NULL && m->image_path == NULL) {
		gabbrev_loader_problem(l, "%s: match `%s' has nothing to type "
		                       "(replace, form, html, markdown or "
		                       "image_path)", where,
		                       m->triggers->len > 0
		                       ? (gchar *)g_ptr_array_index(m->triggers, 0)
		                       : m->regex_text);
		gabbrev_match_unref(m);
		return NULL;
	}
	if (explicit_id != NULL)
		m->id = g_strdup(explicit_id);
	else if (m->triggers->len > 0)
		m->id = g_strdup(g_ptr_array_index(m->triggers, 0));
	else
		m->id = g_strdup(m->regex_text);
	return m;
}

/* ── Documents ───────────────────────────────────────────────────────── */

static void load_file(GabbrevLoader *l, const gchar *path, GPtrArray *closure);
static void load_espanso_dir(GabbrevLoader *l, const gchar *dir);
static void load_any_path(GabbrevLoader *l, const gchar *path,
                          const gchar *base_dir, GPtrArray *closure);

/* Profiles written inline, under `profiles:' */
static void parse_profile_node(GabbrevLoader *l, const gchar *where,
                               YamlNode *node, const gchar *base_dir,
                               const gchar *name);

/* The mapping of a match file or a `gabbrev:' section. */
static void
load_document(
	GabbrevLoader *l,
	YamlMapping   *map,
	const gchar   *where,
	const gchar   *source,
	const gchar   *base_dir,
	GPtrArray     *closure
){
	guint i;

	for (i = 0; i < yaml_mapping_get_size(map); i++) {
		g_autofree gchar *key = norm_key(yaml_mapping_get_key(map, i));
		YamlNode *val = yaml_mapping_get_value(map, i);

		if (g_strcmp0(key, "matches") == 0) {
			YamlSequence *seq;
			guint j;

			if (val == NULL || yaml_node_get_node_type(val) == YAML_NODE_NULL)
				continue;
			seq = yaml_node_get_sequence(val);
			if (seq == NULL) {
				gabbrev_loader_problem(l, "%s: matches is a list", where);
				continue;
			}
			for (j = 0; j < yaml_sequence_get_length(seq); j++) {
				g_autofree gchar *w = g_strdup_printf("%s: match %u", where,
				                                      j + 1);
				GabbrevMatch *m = parse_match(l, w,
					yaml_sequence_get_element(seq, j), source);

				if (m != NULL) {
					m->order = l->order++;
					g_ptr_array_add(l->set->matches, m);
				}
			}
		} else if (g_strcmp0(key, "global_vars") == 0) {
			parse_vars_into(l, where, val, l->set->global_vars);
		} else if (g_strcmp0(key, "imports") == 0 || g_strcmp0(key, "files") == 0) {
			g_autoptr(GPtrArray) paths = get_strings(val);
			guint j;

			for (j = 0; j < paths->len; j++)
				load_any_path(l, g_ptr_array_index(paths, j), base_dir,
				              closure);
		} else if (g_strcmp0(key, "espanso") == 0) {
			const gchar *s = scalar(val);
			gboolean on;

			/* `espanso: true' is espanso's own default location */
			if (parse_bool_text(s, &on)) {
				if (on) {
					g_autofree gchar *d = g_build_filename(
						g_get_user_config_dir(), "espanso", NULL);

					load_espanso_dir(l, d);
				}
			} else if (s != NULL) {
				g_autofree gchar *d = gabbrev_expand_path(s, base_dir);

				load_espanso_dir(l, d);
			}
		} else if (g_strcmp0(key, "profiles") == 0) {
			YamlSequence *seq = val != NULL ? yaml_node_get_sequence(val) : NULL;
			guint j;

			for (j = 0; seq != NULL && j < yaml_sequence_get_length(seq); j++) {
				g_autofree gchar *w = g_strdup_printf("%s: profile %u", where,
				                                      j + 1);
				g_autofree gchar *n = g_strdup_printf("profile-%u", j + 1);

				parse_profile_node(l, w, yaml_sequence_get_element(seq, j),
				                   base_dir, n);
			}
			if (seq == NULL && val != NULL)
				gabbrev_loader_problem(l, "%s: profiles is a list", where);
		} else if (g_strcmp0(key, "anchors") == 0
		           || g_str_has_prefix(key, "x_")) {
			/* YAML anchors live here by espanso convention */
		} else if (!parse_option(l, where, key, val, &l->set->options)) {
			gabbrev_loader_problem(l, "%s: unknown key `%s'", where, key);
		}
	}
}

static void
closure_add(
	GPtrArray   *closure,
	const gchar *path
){
	if (closure != NULL && !g_ptr_array_find_with_equal_func(
		    closure, path, g_str_equal, NULL))
		g_ptr_array_add(closure, g_strdup(path));
}

/*
 * One match file, once.  @closure collects it and everything it
 * imports (an espanso file selection includes imports transitively).
 */
static void
load_file(
	GabbrevLoader *l,
	const gchar   *path,
	GPtrArray     *closure
){
	g_autoptr(YamlParser) parser = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *dir = NULL;
	GPtrArray *mine;
	YamlNode *root;
	YamlMapping *map;
	guint i;

	/* Loaded already: its closure is known */
	mine = g_hash_table_lookup(l->loaded, path);
	if (mine != NULL) {
		for (i = 0; i < mine->len; i++)
			closure_add(closure, g_ptr_array_index(mine, i));
		return;
	}
	if (g_hash_table_contains(l->loading, path)) {
		gabbrev_loader_problem(l, "%s imports itself (through imports:)", path);
		return;
	}
	if (l->depth >= GABBREV_MAX_IMPORT_DEPTH) {
		gabbrev_loader_problem(l, "%s: imports nested deeper than %d", path,
		                       GABBREV_MAX_IMPORT_DEPTH);
		return;
	}

	parser = yaml_parser_new();
	if (!yaml_parser_load_from_file(parser, path, &error)) {
		gabbrev_loader_problem(l, "%s: %s", path, error->message);
		return;
	}
	g_ptr_array_add(l->set->files, g_strdup(path));
	if (!l->in_selection)
		g_hash_table_add(l->always_files, g_strdup(path));

	mine = g_ptr_array_new_with_free_func(g_free);
	g_ptr_array_add(mine, g_strdup(path));
	root = yaml_parser_get_root(parser);
	map = root != NULL ? yaml_node_get_mapping(root) : NULL;
	if (root != NULL && yaml_node_get_node_type(root) != YAML_NODE_NULL
	    && map == NULL) {
		gabbrev_loader_problem(l, "%s: a match file is a mapping", path);
	} else if (map != NULL) {
		dir = g_path_get_dirname(path);
		g_hash_table_add(l->loading, g_strdup(path));
		l->depth++;
		load_document(l, map, path, path, dir, mine);
		l->depth--;
		g_hash_table_remove(l->loading, path);
	}
	g_hash_table_insert(l->loaded, g_strdup(path), mine);
	for (i = 0; i < mine->len; i++)
		closure_add(closure, g_ptr_array_index(mine, i));
}

static gint
compare_paths(
	gconstpointer a,
	gconstpointer b
){
	return g_strcmp0(*(const gchar * const *)a, *(const gchar * const *)b);
}

/* Every file under @dir (recursively), sorted, for globbing. */
static void
walk(
	const gchar *dir,
	GPtrArray   *out,
	guint        depth
){
	g_autoptr(GDir) d = NULL;
	const gchar *name;

	if (depth > 16)
		return;
	d = g_dir_open(dir, 0, NULL);
	if (d == NULL)
		return;
	while ((name = g_dir_read_name(d)) != NULL) {
		g_autofree gchar *p = g_build_filename(dir, name, NULL);

		if (name[0] == '.')
			continue;
		if (g_file_test(p, G_FILE_TEST_IS_DIR))
			walk(p, out, depth + 1);
		else
			g_ptr_array_add(out, g_steal_pointer(&p));
	}
}

static gboolean
is_yaml_name(const gchar *name)
{
	return g_str_has_suffix(name, ".yml") || g_str_has_suffix(name, ".yaml");
}

/*
 * The files @patterns name, globbed relative to @base: the directory
 * part before the first wildcard is walked, the rest matched against
 * each file's path relative to it.  Sorted, so load order is stable.
 */
static GPtrArray *
glob_files(
	GPtrArray   *patterns,
	const gchar *base
){
	GPtrArray *out = g_ptr_array_new_with_free_func(g_free);
	guint i;

	for (i = 0; patterns != NULL && i < patterns->len; i++) {
		g_autofree gchar *full = gabbrev_expand_path(
			g_ptr_array_index(patterns, i), base);
		g_autoptr(GPtrArray) all = NULL;
		const gchar *wild;
		g_autofree gchar *root = NULL;
		guint j;

		wild = strpbrk(full, "*?[");
		if (wild == NULL) {
			if (g_file_test(full, G_FILE_TEST_IS_REGULAR)
			    && !g_ptr_array_find_with_equal_func(out, full,
			                                         g_str_equal, NULL))
				g_ptr_array_add(out, g_strdup(full));
			continue;
		}
		/* The directory part before the wildcard */
		root = g_strndup(full, (gsize)(wild - full));
		if (strrchr(root, '/') != NULL)
			*(strrchr(root, '/') + 1) = '\0';
		all = g_ptr_array_new_with_free_func(g_free);
		walk(root, all, 0);
		g_ptr_array_sort(all, compare_paths);
		for (j = 0; j < all->len; j++) {
			const gchar *p = g_ptr_array_index(all, j);

			if (gabbrev_glob_match(full + strlen(root), p + strlen(root))
			    && !g_ptr_array_find_with_equal_func(out, p, g_str_equal,
			                                         NULL))
				g_ptr_array_add(out, g_strdup(p));
		}
	}
	return out;
}

/* A directory that is not an espanso configuration: every YAML file in
   it, recursively, except those starting with `_' (espanso's rule for
   "only when imported"). */
static void
load_plain_dir(
	GabbrevLoader *l,
	const gchar   *dir,
	GPtrArray     *closure
){
	g_autoptr(GPtrArray) all = g_ptr_array_new_with_free_func(g_free);
	guint i;

	walk(dir, all, 0);
	g_ptr_array_sort(all, compare_paths);
	for (i = 0; i < all->len; i++) {
		const gchar *p = g_ptr_array_index(all, i);
		g_autofree gchar *base = g_path_get_basename(p);

		if (is_yaml_name(base) && base[0] != '_')
			load_file(l, p, closure);
	}
}

static void
load_any_path(
	GabbrevLoader *l,
	const gchar   *path,
	const gchar   *base_dir,
	GPtrArray     *closure
){
	g_autofree gchar *full = gabbrev_expand_path(path, base_dir);

	if (strpbrk(full, "*?[") != NULL) {
		g_autoptr(GPtrArray) pats = g_ptr_array_new_with_free_func(g_free);
		g_autoptr(GPtrArray) files = NULL;
		guint i;

		g_ptr_array_add(pats, g_strdup(full));
		files = glob_files(pats, NULL);
		for (i = 0; i < files->len; i++)
			load_file(l, g_ptr_array_index(files, i), closure);
		return;
	}
	if (g_file_test(full, G_FILE_TEST_IS_DIR)) {
		g_autofree gchar *cfg = g_build_filename(full, "config", NULL);
		g_autofree gchar *mt = g_build_filename(full, "match", NULL);

		if (g_file_test(cfg, G_FILE_TEST_IS_DIR)
		    || g_file_test(mt, G_FILE_TEST_IS_DIR))
			load_espanso_dir(l, full);
		else
			load_plain_dir(l, full, closure);
		return;
	}
	if (!g_file_test(full, G_FILE_TEST_EXISTS)) {
		gabbrev_loader_problem(l, "%s does not exist", full);
		return;
	}
	load_file(l, full, closure);
}

/* ── Profiles ────────────────────────────────────────────────────────── */

/* What an espanso config file says about file selection. */
typedef struct {
	GPtrArray *includes;        /* NULL: not given */
	GPtrArray *excludes;
	GPtrArray *extra_includes;
	GPtrArray *extra_excludes;
} Selection;

static void
selection_clear(Selection *s)
{
	g_clear_pointer(&s->includes, g_ptr_array_unref);
	g_clear_pointer(&s->excludes, g_ptr_array_unref);
	g_clear_pointer(&s->extra_includes, g_ptr_array_unref);
	g_clear_pointer(&s->extra_excludes, g_ptr_array_unref);
}

/*
 * The files a selection puts in force, loaded, with their imports:
 * includes (or @fallback when not given) and extra_includes, minus
 * excludes and extra_excludes.
 */
static GHashTable *
resolve_selection(
	GabbrevLoader   *l,
	const Selection *sel,
	GPtrArray       *fallback,
	const gchar     *base
){
	g_autoptr(GPtrArray) inc = NULL;
	g_autoptr(GPtrArray) extra = NULL;
	g_autoptr(GPtrArray) exc = NULL;
	g_autoptr(GPtrArray) exc2 = NULL;
	g_autoptr(GPtrArray) closure = g_ptr_array_new_with_free_func(g_free);
	GHashTable *files;
	guint i;

	inc = glob_files(sel->includes != NULL ? sel->includes : fallback, base);
	extra = glob_files(sel->extra_includes, base);
	exc = glob_files(sel->excludes, base);
	exc2 = glob_files(sel->extra_excludes, base);

	l->in_selection = TRUE;
	for (i = 0; i < inc->len; i++) {
		const gchar *p = g_ptr_array_index(inc, i);

		if (!g_ptr_array_find_with_equal_func(exc, p, g_str_equal, NULL)
		    && !g_ptr_array_find_with_equal_func(exc2, p, g_str_equal, NULL))
			load_file(l, p, closure);
	}
	for (i = 0; i < extra->len; i++) {
		const gchar *p = g_ptr_array_index(extra, i);

		if (!g_ptr_array_find_with_equal_func(exc2, p, g_str_equal, NULL))
			load_file(l, p, closure);
	}
	l->in_selection = FALSE;

	files = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	for (i = 0; i < closure->len; i++)
		g_hash_table_add(files, g_strdup(g_ptr_array_index(closure, i)));
	return files;
}

/*
 * The keys of an espanso config file (or an inline profile) that are
 * not options: filters and file selection.  Returns FALSE for a key it
 * does not know.
 */
static gboolean
parse_profile_key(
	GabbrevLoader  *l,
	const gchar    *where,
	const gchar    *key,
	YamlNode       *val,
	GabbrevProfile *p,
	Selection      *sel
){
	if (g_strcmp0(key, "filter_title") == 0)
		p->filter_title = get_regex(l, where, key, val);
	else if (g_strcmp0(key, "filter_class") == 0)
		p->filter_class = get_regex(l, where, key, val);
	else if (g_strcmp0(key, "filter_exec") == 0)
		p->filter_exec = get_regex(l, where, key, val);
	else if (g_strcmp0(key, "filter_os") == 0)
		p->filter_os = g_strdup(scalar(val));
	else if (g_strcmp0(key, "filter") == 0)
		p->filter = get_filter(l, where, val, &p->filter_text);
	else if (g_strcmp0(key, "includes") == 0)
		sel->includes = get_strings(val);
	else if (g_strcmp0(key, "excludes") == 0)
		sel->excludes = get_strings(val);
	else if (g_strcmp0(key, "extra_includes") == 0)
		sel->extra_includes = get_strings(val);
	else if (g_strcmp0(key, "extra_excludes") == 0)
		sel->extra_excludes = get_strings(val);
	else if (g_strcmp0(key, "name") == 0) {
		g_free(p->name);
		p->name = g_strdup(scalar(val));
	} else
		return parse_option(l, where, key, val, &p->options);
	return TRUE;
}

static gboolean
selection_given(const Selection *s)
{
	return s->includes != NULL || s->excludes != NULL
	       || s->extra_includes != NULL || s->extra_excludes != NULL;
}

static void
parse_profile_node(
	GabbrevLoader *l,
	const gchar   *where,
	YamlNode      *node,
	const gchar   *base_dir,
	const gchar   *name
){
	YamlMapping *map = node != NULL ? yaml_node_get_mapping(node) : NULL;
	GabbrevProfile *p;
	Selection sel;
	guint i;

	if (map == NULL) {
		gabbrev_loader_problem(l, "%s: a profile is a mapping", where);
		return;
	}
	memset(&sel, 0, sizeof sel);
	p = g_new0(GabbrevProfile, 1);
	p->name = g_strdup(name);
	for (i = 0; i < yaml_mapping_get_size(map); i++) {
		g_autofree gchar *key = norm_key(yaml_mapping_get_key(map, i));

		if (!parse_profile_key(l, where, key, yaml_mapping_get_value(map, i),
		                       p, &sel))
			gabbrev_loader_problem(l, "%s: unknown key `%s'", where, key);
	}
	if (selection_given(&sel))
		p->files = resolve_selection(l, &sel, NULL, base_dir);
	selection_clear(&sel);
	g_ptr_array_add(l->set->profiles, p);
}

/*
 * An espanso configuration directory: config/default.yml, the
 * app-specific config/NAME.yml, and the match files they select.
 */
static void
load_espanso_dir(
	GabbrevLoader *l,
	const gchar   *dir
){
	g_autofree gchar *cfg = g_build_filename(dir, "config", NULL);
	g_autoptr(GPtrArray) fallback = g_ptr_array_new_with_free_func(g_free);
	g_autoptr(GPtrArray) cfg_files = g_ptr_array_new_with_free_func(g_free);
	Selection def;
	GHashTable *def_files;
	g_autoptr(GDir) d = NULL;
	const gchar *name;
	guint i;

	if (!g_file_test(dir, G_FILE_TEST_IS_DIR)) {
		gabbrev_loader_problem(l, "%s: no espanso configuration there", dir);
		return;
	}
	g_ptr_array_add(fallback, g_strdup("../match/**/[!_]*.yml"));
	g_ptr_array_add(fallback, g_strdup("../match/**/[!_]*.yaml"));
	memset(&def, 0, sizeof def);

	/* config/NAME.yml, default first */
	d = g_dir_open(cfg, 0, NULL);
	while (d != NULL && (name = g_dir_read_name(d)) != NULL)
		if (is_yaml_name(name))
			g_ptr_array_add(cfg_files, g_build_filename(cfg, name, NULL));
	g_ptr_array_sort(cfg_files, compare_paths);

	for (i = 0; i < cfg_files->len; i++) {
		const gchar *p = g_ptr_array_index(cfg_files, i);
		g_autofree gchar *base = g_path_get_basename(p);
		g_autoptr(YamlParser) parser = NULL;
		g_autoptr(GError) error = NULL;
		YamlMapping *map;
		guint k;

		if (!g_str_has_prefix(base, "default."))
			continue;
		parser = yaml_parser_new();
		if (!yaml_parser_load_from_file(parser, p, &error)) {
			gabbrev_loader_problem(l, "%s: %s", p, error->message);
			continue;
		}
		g_ptr_array_add(l->set->files, g_strdup(p));
		map = yaml_parser_get_root(parser) != NULL
			? yaml_node_get_mapping(yaml_parser_get_root(parser)) : NULL;
		for (k = 0; map != NULL && k < yaml_mapping_get_size(map); k++) {
			g_autofree gchar *key = norm_key(yaml_mapping_get_key(map, k));
			YamlNode *val = yaml_mapping_get_value(map, k);
			GabbrevProfile scratch;

			memset(&scratch, 0, sizeof scratch);
			/* default.yml has no filters; its options are the base */
			if (g_str_has_prefix(key, "filter_")) {
				gabbrev_loader_problem(l, "%s: default.yml cannot have "
				                       "`%s'", p, key);
				continue;
			}
			if (!parse_profile_key(l, p, key, val, &scratch, &def))
				gabbrev_loader_problem(l, "%s: unknown key `%s'", p, key);
			gabbrev_options_merge(&l->set->options, &scratch.options);
			gabbrev_options_clear(&scratch.options);
			g_free(scratch.name);
		}
	}

	def_files = resolve_selection(l, &def, fallback, cfg);
	if (l->default_files == NULL)
		l->default_files = def_files;
	else {
		GHashTableIter it;
		gpointer k;

		g_hash_table_iter_init(&it, def_files);
		while (g_hash_table_iter_next(&it, &k, NULL))
			g_hash_table_add(l->default_files, g_strdup(k));
		g_hash_table_unref(def_files);
	}

	/* The app-specific ones */
	for (i = 0; i < cfg_files->len; i++) {
		const gchar *p = g_ptr_array_index(cfg_files, i);
		g_autofree gchar *base = g_path_get_basename(p);
		g_autoptr(YamlParser) parser = NULL;
		g_autoptr(GError) error = NULL;
		YamlMapping *map;
		GabbrevProfile *prof;
		Selection sel;
		guint k;

		if (g_str_has_prefix(base, "default."))
			continue;
		parser = yaml_parser_new();
		if (!yaml_parser_load_from_file(parser, p, &error)) {
			gabbrev_loader_problem(l, "%s: %s", p, error->message);
			continue;
		}
		g_ptr_array_add(l->set->files, g_strdup(p));
		map = yaml_parser_get_root(parser) != NULL
			? yaml_node_get_mapping(yaml_parser_get_root(parser)) : NULL;
		memset(&sel, 0, sizeof sel);
		prof = g_new0(GabbrevProfile, 1);
		prof->name = g_strndup(base, (gsize)(strrchr(base, '.') - base));
		for (k = 0; map != NULL && k < yaml_mapping_get_size(map); k++) {
			g_autofree gchar *key = norm_key(yaml_mapping_get_key(map, k));

			if (!parse_profile_key(l, p, key, yaml_mapping_get_value(map, k),
			                       prof, &sel))
				gabbrev_loader_problem(l, "%s: unknown key `%s'", p, key);
		}
		/* espanso: an app config selects the default's files unless it
		   names its own, plus its extras */
		if (sel.includes == NULL && def.includes != NULL)
			sel.includes = g_ptr_array_ref(def.includes);
		prof->files = resolve_selection(l, &sel, fallback, cfg);
		selection_clear(&sel);
		g_ptr_array_add(l->set->profiles, prof);
	}
	selection_clear(&def);
}

/* ── Public loader API ───────────────────────────────────────────────── */

/**
 * gabbrev_loader_add_builtin: (skip)
 * @loader: a loader
 *
 * The matches gabbrev ships (`;today', `;shrug', ...), first, so any
 * source after them overrides one by giving the same trigger.
 */
void
gabbrev_loader_add_builtin(GabbrevLoader *loader)
{
	gabbrev_loader_add_text(loader, gabbrev_builtin_yaml(), NULL,
	                        GABBREV_ORIGIN_BUILTIN);
}

/**
 * gabbrev_loader_add_path: (skip)
 * @loader: a loader
 * @path: a match file, a directory of them, an espanso configuration
 *   directory, or a glob
 * @base_dir: (nullable): what a relative @path is relative to
 */
void
gabbrev_loader_add_path(
	GabbrevLoader *loader,
	const gchar   *path,
	const gchar   *base_dir
){
	load_any_path(loader, path, base_dir, NULL);
}

/**
 * gabbrev_loader_add_node: (skip)
 * @loader: a loader
 * @mapping: a `gabbrev:' section or a match file's root
 * @base_dir: (nullable): what its relative paths are relative to
 * @origin: where it came from (a GABBREV_ORIGIN_*), for its matches
 */
void
gabbrev_loader_add_node(
	GabbrevLoader *loader,
	YamlNode      *mapping,
	const gchar   *base_dir,
	const gchar   *origin
){
	YamlMapping *map;

	if (mapping == NULL || yaml_node_get_node_type(mapping) == YAML_NODE_NULL)
		return;
	map = yaml_node_get_mapping(mapping);
	if (map == NULL) {
		gabbrev_loader_problem(loader, "%s: not a mapping", origin);
		return;
	}
	load_document(loader, map, origin, origin, base_dir, NULL);
}

/**
 * gabbrev_loader_add_text: (skip)
 * @loader: a loader
 * @yaml: a match file's text (JSON is YAML too)
 * @base_dir: (nullable): what its relative paths are relative to
 * @origin: where it came from
 *
 * Returns: %FALSE when @yaml does not parse (a problem is recorded)
 */
gboolean
gabbrev_loader_add_text(
	GabbrevLoader *loader,
	const gchar   *yaml,
	const gchar   *base_dir,
	const gchar   *origin
){
	g_autoptr(YamlParser) parser = yaml_parser_new();
	g_autoptr(GError) error = NULL;

	if (!yaml_parser_load_from_data(parser, yaml, -1, &error)) {
		gabbrev_loader_problem(loader, "%s: %s", origin, error->message);
		return FALSE;
	}
	gabbrev_loader_add_node(loader, yaml_parser_get_root(parser), base_dir,
	                        origin);
	return TRUE;
}

/**
 * gabbrev_loader_finish: (skip)
 * @loader: a loader; empty afterwards
 *
 * Settles which files each profile allows -- once an espanso file
 * selection exists, the default profile allows only that selection
 * plus files loaded outside it (set->default_files), and so does every
 * profile with one of its own -- and hands the set over.
 *
 * Returns: (transfer full): the set
 */
GabbrevSet *
gabbrev_loader_finish(GabbrevLoader *loader)
{
	GabbrevSet *set;
	GHashTableIter it;
	gpointer k;
	guint i;

	set = g_steal_pointer(&loader->set);
	loader->set = set_new();

	for (i = 0; i < set->profiles->len; i++) {
		GabbrevProfile *p = g_ptr_array_index(set->profiles, i);

		if (p->files == NULL)
			continue;
		g_hash_table_iter_init(&it, loader->always_files);
		while (g_hash_table_iter_next(&it, &k, NULL))
			g_hash_table_add(p->files, g_strdup(k));
	}
	if (loader->default_files != NULL) {
		set->default_files = g_steal_pointer(&loader->default_files);
		g_hash_table_iter_init(&it, loader->always_files);
		while (g_hash_table_iter_next(&it, &k, NULL))
			g_hash_table_add(set->default_files, g_strdup(k));
	}
	return set;
}
