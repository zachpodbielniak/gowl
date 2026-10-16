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
 * The pure half of the `devservices' widget; see bar-devservices-model.h.
 *
 * Nothing in this file knows about the bar.  The plugin file calls in
 * from its worker, and tests/test-bar-devservices.c compiles this file
 * straight into the test, with a fabricated /proc where one matters.
 */

#include "bar-devservices-model.h"
#include "bar-util.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <glib/gstdio.h>

#include "yaml-glib.h"
#include "barkit/gowl-bar-json.h"

G_DEFINE_QUARK(bar-devservices-error-quark, bar_devservices_error)

/* ----------------------------------------------------------------
 * Small helpers
 * ---------------------------------------------------------------- */

/*
 * read_bounded:
 * @path: a file
 * @max: the most bytes it may have
 *
 * g_file_get_contents() with a size check first: the stack directory
 * and a scanned project are user territory, and a 2 GiB file named
 * package.json must cost a stat(), not the session's memory.
 *
 * Returns: (transfer full) (nullable): the contents
 */
static gchar *
read_bounded(const gchar *path, goffset max)
{
	GStatBuf st;
	gchar *text = NULL;

	if (g_stat(path, &st) != 0 || !S_ISREG(st.st_mode) ||
	    (goffset)st.st_size > max)
		return NULL;
	if (!g_file_get_contents(path, &text, NULL, NULL))
		return NULL;
	return text;
}

/*
 * expand_tilde:
 * @path: a path that may start with `~'
 *
 * The model's own copy of bar_expand_tilde(): that one lives in the
 * module file, which the test does not link.
 *
 * Returns: (transfer full): the expanded path
 */
static gchar *
expand_tilde(const gchar *path)
{
	if (path == NULL)
		return NULL;
	if (path[0] == '~' && (path[1] == '/' || path[1] == '\0'))
		return g_build_filename(g_get_home_dir(), path + 1, NULL);
	return g_strdup(path);
}

/*
 * resolve:
 * @path: a path
 *
 * realpath() when the path exists, so two spellings of the same
 * directory (a symlinked ~/src, a trailing slash, `..') compare equal;
 * a lexical canonicalisation when it does not.
 *
 * Returns: (transfer full): the resolved path
 */
static gchar *
resolve(const gchar *path)
{
	char buf[PATH_MAX];

	if (realpath(path, buf) != NULL)
		return g_strdup(buf);
	return g_canonicalize_filename(path, "/");
}

gboolean
bar_devservices_valid_id(const gchar *id)
{
	const gchar *p;

	if (id == NULL || id[0] == '\0' || strlen(id) > 64)
		return FALSE;
	if (strcmp(id, ".") == 0 || strcmp(id, "..") == 0)
		return FALSE;
	for (p = id; *p != '\0'; p++) {
		if (!g_ascii_isalnum(*p) && *p != '_' && *p != '.' && *p != '-')
			return FALSE;
	}
	return TRUE;
}

/* A run id is a service id or `task:' followed by a task id. */
static gboolean
valid_run_id(const gchar *id)
{
	if (id != NULL && g_str_has_prefix(id, "task:"))
		return bar_devservices_valid_id(id + 5);
	return bar_devservices_valid_id(id);
}

gchar *
bar_devservices_safe_key(const gchar *name)
{
	GString *out;
	const gchar *p;

	out = g_string_new(NULL);
	for (p = (name != NULL) ? name : ""; *p != '\0' && out->len < 48; p++) {
		gchar c = g_ascii_tolower(*p);

		if (g_ascii_isalnum(c) || c == '_' || c == '.' || c == '-')
			g_string_append_c(out, c);
		else if (out->len > 0 && out->str[out->len - 1] != '-')
			g_string_append_c(out, '-');
	}
	while (out->len > 0 && out->str[out->len - 1] == '-')
		g_string_truncate(out, out->len - 1);
	if (out->len == 0 || strcmp(out->str, ".") == 0 ||
	    strcmp(out->str, "..") == 0)
		g_string_assign(out, "project");
	return g_string_free(out, FALSE);
}

/* ----------------------------------------------------------------
 * Stack definitions
 * ---------------------------------------------------------------- */

static void
service_free(gpointer data)
{
	BarDevservicesService *svc = data;

	if (svc == NULL)
		return;
	g_free(svc->id);
	g_free(svc->label);
	g_free(svc->dir);
	g_strfreev(svc->argv);
	g_free(svc->health);
	g_free(svc->url);
	g_strfreev(svc->env);
	g_free(svc->compose);
	g_free(svc->compose_service);
	g_free(svc->compose_project);
	g_strfreev(svc->needs);
	g_free(svc);
}

static void
task_free(gpointer data)
{
	BarDevservicesTask *task = data;

	if (task == NULL)
		return;
	g_free(task->id);
	g_free(task->label);
	g_free(task->dir);
	g_strfreev(task->argv);
	g_strfreev(task->needs);
	g_free(task);
}

/**
 * bar_devservices_stack_free:
 * @stack: (nullable) (transfer full): a stack
 */
void
bar_devservices_stack_free(BarDevservicesStack *stack)
{
	if (stack == NULL)
		return;
	g_free(stack->key);
	g_free(stack->name);
	g_free(stack->root);
	g_free(stack->file);
	g_ptr_array_unref(stack->services);
	g_ptr_array_unref(stack->tasks);
	g_free(stack);
}

/* The scalar text of @node, or NULL when it is not a scalar. */
static const gchar *
node_scalar(YamlNode *node)
{
	if (node == NULL || yaml_node_get_node_type(node) != YAML_NODE_SCALAR)
		return NULL;
	return yaml_node_get_scalar(node);
}

static YamlNode *
member(YamlMapping *map, const gchar *key)
{
	if (map == NULL || !yaml_mapping_has_member(map, key))
		return NULL;
	return yaml_mapping_get_member(map, key);
}

static const gchar *
scalar_member(YamlMapping *map, const gchar *key)
{
	return node_scalar(member(map, key));
}

/*
 * parse_bool:
 * @text: (nullable): a YAML scalar
 * @fallback: the value for %NULL
 * @ok: (out): %FALSE when @text is not a boolean spelling
 *
 * YAML 1.1 spellings, because that is what people write by hand.
 */
static gboolean
parse_bool(const gchar *text, gboolean fallback, gboolean *ok)
{
	*ok = TRUE;
	if (text == NULL)
		return fallback;
	if (g_ascii_strcasecmp(text, "true") == 0 ||
	    g_ascii_strcasecmp(text, "yes") == 0 ||
	    g_ascii_strcasecmp(text, "on") == 0 || strcmp(text, "1") == 0)
		return TRUE;
	if (g_ascii_strcasecmp(text, "false") == 0 ||
	    g_ascii_strcasecmp(text, "no") == 0 ||
	    g_ascii_strcasecmp(text, "off") == 0 || strcmp(text, "0") == 0)
		return FALSE;
	*ok = FALSE;
	return fallback;
}

/*
 * parse_int:
 * @text: (nullable): a YAML scalar
 * @lo: the smallest accepted value
 * @hi: the largest
 * @fallback: the value for %NULL
 * @ok: (out): %FALSE when @text is not a whole number in range
 */
static gint
parse_int(const gchar *text, gint lo, gint hi, gint fallback, gboolean *ok)
{
	gint64 v;
	gchar *end = NULL;

	*ok = TRUE;
	if (text == NULL)
		return fallback;
	v = g_ascii_strtoll(text, &end, 10);
	if (end == text || *end != '\0' || v < lo || v > hi) {
		*ok = FALSE;
		return fallback;
	}
	return (gint)v;
}

/*
 * parse_cmd:
 * @node: the `cmd' value
 * @what: the owner, for messages
 * @shell: (out): whether it was the string form
 * @error: return location for a #GError
 *
 * A YAML list is the argv as written -- the preferred form, since no
 * shell sees it and nothing in it needs quoting.  A string is handed
 * to `sh -c' whole, which is what someone writing `npm run dev | tee
 * x' means; it is NOT split on spaces, which would hand the pipe to
 * npm as an argument.
 *
 * Returns: (transfer full) (nullable): the argv
 */
static gchar **
parse_cmd(YamlNode *node, const gchar *what, gboolean *shell, GError **error)
{
	YamlSequence *seq;
	GPtrArray *argv;
	guint i, n;
	const gchar *text;

	*shell = FALSE;
	if (node == NULL) {
		g_set_error(error, BAR_DEVSERVICES_ERROR,
		            BAR_DEVSERVICES_ERROR_INVALID, "%s has no cmd", what);
		return NULL;
	}

	text = node_scalar(node);
	if (text != NULL) {
		gchar **out;

		if (text[0] == '\0') {
			g_set_error(error, BAR_DEVSERVICES_ERROR,
			            BAR_DEVSERVICES_ERROR_INVALID,
			            "%s has an empty cmd", what);
			return NULL;
		}
		out = g_new0(gchar *, 4);
		out[0] = g_strdup("sh");
		out[1] = g_strdup("-c");
		out[2] = g_strdup(text);
		*shell = TRUE;
		return out;
	}

	if (yaml_node_get_node_type(node) != YAML_NODE_SEQUENCE ||
	    (seq = yaml_node_get_sequence(node)) == NULL) {
		g_set_error(error, BAR_DEVSERVICES_ERROR,
		            BAR_DEVSERVICES_ERROR_INVALID,
		            "%s: cmd must be a list of words or a string", what);
		return NULL;
	}

	n = yaml_sequence_get_length(seq);
	if (n == 0 || n > 256) {
		g_set_error(error, BAR_DEVSERVICES_ERROR,
		            BAR_DEVSERVICES_ERROR_INVALID,
		            "%s: cmd must have 1 to 256 words", what);
		return NULL;
	}
	argv = g_ptr_array_new_with_free_func(g_free);
	for (i = 0; i < n; i++) {
		const gchar *word = node_scalar(yaml_sequence_get_element(seq, i));

		if (word == NULL) {
			g_ptr_array_unref(argv);
			g_set_error(error, BAR_DEVSERVICES_ERROR,
			            BAR_DEVSERVICES_ERROR_INVALID,
			            "%s: cmd word %u is not a string", what, i + 1);
			return NULL;
		}
		g_ptr_array_add(argv, g_strdup(word));
	}
	if (((const gchar *)g_ptr_array_index(argv, 0))[0] == '\0') {
		g_ptr_array_unref(argv);
		g_set_error(error, BAR_DEVSERVICES_ERROR,
		            BAR_DEVSERVICES_ERROR_INVALID,
		            "%s: cmd starts with an empty word", what);
		return NULL;
	}
	g_ptr_array_add(argv, NULL);
	g_ptr_array_set_free_func(argv, NULL);
	return (gchar **)g_ptr_array_free(argv, FALSE);
}

/*
 * parse_ids:
 * @node: (nullable): a list of ids
 * @what: the owner, for messages
 * @error: return location for a #GError
 *
 * Returns: (transfer full): the ids, an empty vector when @node is
 *   absent, or %NULL on error
 */
static gchar **
parse_ids(YamlNode *node, const gchar *what, GError **error)
{
	YamlSequence *seq;
	GPtrArray *out;
	guint i, n;

	out = g_ptr_array_new_with_free_func(g_free);
	if (node != NULL && yaml_node_get_node_type(node) != YAML_NODE_NULL) {
		seq = (yaml_node_get_node_type(node) == YAML_NODE_SEQUENCE)
			? yaml_node_get_sequence(node) : NULL;
		if (seq == NULL) {
			g_ptr_array_unref(out);
			g_set_error(error, BAR_DEVSERVICES_ERROR,
			            BAR_DEVSERVICES_ERROR_INVALID,
			            "%s: needs must be a list of service ids", what);
			return NULL;
		}
		n = yaml_sequence_get_length(seq);
		for (i = 0; i < n; i++) {
			const gchar *id;

			id = node_scalar(yaml_sequence_get_element(seq, i));
			if (!bar_devservices_valid_id(id)) {
				g_ptr_array_unref(out);
				g_set_error(error, BAR_DEVSERVICES_ERROR,
				            BAR_DEVSERVICES_ERROR_INVALID,
				            "%s: needs entry %u is not a service id",
				            what, i + 1);
				return NULL;
			}
			g_ptr_array_add(out, g_strdup(id));
		}
	}
	g_ptr_array_add(out, NULL);
	g_ptr_array_set_free_func(out, NULL);
	return (gchar **)g_ptr_array_free(out, FALSE);
}

/*
 * parse_env:
 * @node: (nullable): a mapping of names to values
 * @what: the owner, for messages
 * @error: return location for a #GError
 *
 * Names must be shell-variable shaped: an `=' in a name would split
 * somewhere else than the user meant.
 *
 * Returns: (transfer full): `KEY=VALUE' entries, or %NULL on error
 */
static gchar **
parse_env(YamlNode *node, const gchar *what, GError **error)
{
	YamlMapping *map;
	GPtrArray *out;
	guint i, n;

	out = g_ptr_array_new_with_free_func(g_free);
	if (node != NULL && yaml_node_get_node_type(node) != YAML_NODE_NULL) {
		map = (yaml_node_get_node_type(node) == YAML_NODE_MAPPING)
			? yaml_node_get_mapping(node) : NULL;
		if (map == NULL) {
			g_ptr_array_unref(out);
			g_set_error(error, BAR_DEVSERVICES_ERROR,
			            BAR_DEVSERVICES_ERROR_INVALID,
			            "%s: env must be a mapping", what);
			return NULL;
		}
		n = yaml_mapping_get_size(map);
		for (i = 0; i < n; i++) {
			const gchar *k = yaml_mapping_get_key(map, i);
			const gchar *v = node_scalar(yaml_mapping_get_value(map, i));
			const gchar *p;
			gboolean ok = (k != NULL && k[0] != '\0' &&
			               !g_ascii_isdigit(k[0]));

			for (p = k; ok && *p != '\0'; p++)
				if (!g_ascii_isalnum(*p) && *p != '_')
					ok = FALSE;
			if (!ok) {
				g_ptr_array_unref(out);
				g_set_error(error, BAR_DEVSERVICES_ERROR,
				            BAR_DEVSERVICES_ERROR_INVALID,
				            "%s: env name %u is not a variable name",
				            what, i + 1);
				return NULL;
			}
			g_ptr_array_add(out, g_strdup_printf("%s=%s", k,
			                (v != NULL) ? v : ""));
		}
	}
	g_ptr_array_add(out, NULL);
	g_ptr_array_set_free_func(out, NULL);
	return (gchar **)g_ptr_array_free(out, FALSE);
}

/* @rel against @root, unless it is already absolute. */
static gchar *
under_root(const gchar *root, const gchar *rel)
{
	g_autofree gchar *expanded = NULL;

	if (rel == NULL || rel[0] == '\0')
		return g_strdup(root);
	expanded = expand_tilde(rel);
	if (g_path_is_absolute(expanded))
		return g_steal_pointer(&expanded);
	return g_build_filename(root, expanded, NULL);
}

/*
 * parse_service:
 * @map: one entry of `services'
 * @root: the stack's root
 * @index: its position, for messages
 * @error: return location for a #GError
 *
 * Returns: (transfer full) (nullable): the service
 */
static BarDevservicesService *
parse_service(YamlMapping *map, const gchar *root, guint index, GError **error)
{
	BarDevservicesService *svc;
	const gchar *id, *type, *text;
	g_autofree gchar *what = NULL;
	gboolean ok;

	id = scalar_member(map, "id");
	if (!bar_devservices_valid_id(id)) {
		g_set_error(error, BAR_DEVSERVICES_ERROR,
		            BAR_DEVSERVICES_ERROR_INVALID,
		            "service %u needs an id of letters, digits, `_', "
		            "`.' and `-'", index + 1);
		return NULL;
	}
	what = g_strdup_printf("service `%s'", id);

	svc = g_new0(BarDevservicesService, 1);
	svc->id = g_strdup(id);
	text = scalar_member(map, "label");
	svc->label = g_utf8_make_valid((text != NULL && text[0] != '\0')
	                               ? text : id, -1);
	svc->dir = under_root(root, scalar_member(map, "dir"));

	type = scalar_member(map, "type");
	if (type == NULL || strcmp(type, "process") == 0) {
		svc->kind = BAR_DEVSERVICES_KIND_PROCESS;
	} else if (strcmp(type, "podman") == 0) {
		svc->kind = BAR_DEVSERVICES_KIND_PODMAN;
	} else {
		g_set_error(error, BAR_DEVSERVICES_ERROR,
		            BAR_DEVSERVICES_ERROR_INVALID,
		            "%s: type must be process or podman", what);
		service_free(svc);
		return NULL;
	}

	if (svc->kind == BAR_DEVSERVICES_KIND_PROCESS) {
		svc->argv = parse_cmd(member(map, "cmd"), what, &svc->shell,
		                      error);
		if (svc->argv == NULL) {
			service_free(svc);
			return NULL;
		}
	} else {
		text = scalar_member(map, "compose");
		if (text == NULL || text[0] == '\0') {
			g_set_error(error, BAR_DEVSERVICES_ERROR,
			            BAR_DEVSERVICES_ERROR_INVALID,
			            "%s: a podman service needs `compose' (the "
			            "compose file)", what);
			service_free(svc);
			return NULL;
		}
		svc->compose = under_root(root, text);
		text = scalar_member(map, "service");
		if (text != NULL && !bar_devservices_valid_id(text)) {
			g_set_error(error, BAR_DEVSERVICES_ERROR,
			            BAR_DEVSERVICES_ERROR_INVALID,
			            "%s: `service' is not a compose service name",
			            what);
			service_free(svc);
			return NULL;
		}
		svc->compose_service = g_strdup((text != NULL) ? text : id);
		text = scalar_member(map, "project");
		if (text != NULL && !bar_devservices_valid_id(text)) {
			g_set_error(error, BAR_DEVSERVICES_ERROR,
			            BAR_DEVSERVICES_ERROR_INVALID,
			            "%s: `project' is not a compose project name",
			            what);
			service_free(svc);
			return NULL;
		}
		svc->compose_project = g_strdup(text);
	}

	svc->port = parse_int(scalar_member(map, "port"), 1, 65535, 0, &ok);
	if (!ok) {
		g_set_error(error, BAR_DEVSERVICES_ERROR,
		            BAR_DEVSERVICES_ERROR_INVALID,
		            "%s: port must be 1-65535", what);
		service_free(svc);
		return NULL;
	}
	svc->wait = parse_int(scalar_member(map, "wait"), 1, 3600, 60, &ok);
	if (!ok) {
		g_set_error(error, BAR_DEVSERVICES_ERROR,
		            BAR_DEVSERVICES_ERROR_INVALID,
		            "%s: wait must be 1-3600 seconds", what);
		service_free(svc);
		return NULL;
	}
	svc->open = parse_bool(scalar_member(map, "open"), FALSE, &ok);
	if (!ok) {
		g_set_error(error, BAR_DEVSERVICES_ERROR,
		            BAR_DEVSERVICES_ERROR_INVALID,
		            "%s: open must be true or false", what);
		service_free(svc);
		return NULL;
	}

	text = scalar_member(map, "health");
	if (text != NULL && text[0] != '\0') {
		if (strcmp(text, "port") != 0 &&
		    !g_str_has_prefix(text, "http://") &&
		    !g_str_has_prefix(text, "https://")) {
			g_set_error(error, BAR_DEVSERVICES_ERROR,
			            BAR_DEVSERVICES_ERROR_INVALID,
			            "%s: health must be `port' or an http(s) URL",
			            what);
			service_free(svc);
			return NULL;
		}
		if (strcmp(text, "port") == 0 && svc->port == 0) {
			g_set_error(error, BAR_DEVSERVICES_ERROR,
			            BAR_DEVSERVICES_ERROR_INVALID,
			            "%s: health is `port' but no port is set", what);
			service_free(svc);
			return NULL;
		}
		svc->health = g_strdup(text);
	}
	text = scalar_member(map, "url");
	if (text != NULL && text[0] != '\0') {
		/* The URL is handed to xdg-open; anything that is not a web
		   address could open a file or run a handler for some
		   other scheme. */
		if (!g_str_has_prefix(text, "http://") &&
		    !g_str_has_prefix(text, "https://")) {
			g_set_error(error, BAR_DEVSERVICES_ERROR,
			            BAR_DEVSERVICES_ERROR_INVALID,
			            "%s: url must be http:// or https://", what);
			service_free(svc);
			return NULL;
		}
		svc->url = g_strdup(text);
	}

	svc->env = parse_env(member(map, "env"), what, error);
	if (svc->env == NULL) {
		service_free(svc);
		return NULL;
	}
	svc->needs = parse_ids(member(map, "needs"), what, error);
	if (svc->needs == NULL) {
		service_free(svc);
		return NULL;
	}
	return svc;
}

static BarDevservicesTask *
parse_task(YamlMapping *map, const gchar *root, guint index, GError **error)
{
	BarDevservicesTask *task;
	const gchar *id, *text;
	g_autofree gchar *what = NULL;
	gboolean shell;

	id = scalar_member(map, "id");
	if (!bar_devservices_valid_id(id)) {
		g_set_error(error, BAR_DEVSERVICES_ERROR,
		            BAR_DEVSERVICES_ERROR_INVALID,
		            "task %u needs an id of letters, digits, `_', `.' "
		            "and `-'", index + 1);
		return NULL;
	}
	what = g_strdup_printf("task `%s'", id);

	task = g_new0(BarDevservicesTask, 1);
	task->id = g_strdup(id);
	text = scalar_member(map, "label");
	task->label = g_utf8_make_valid((text != NULL && text[0] != '\0')
	                                ? text : id, -1);
	task->dir = under_root(root, scalar_member(map, "dir"));
	task->argv = parse_cmd(member(map, "cmd"), what, &shell, error);
	if (task->argv == NULL) {
		task_free(task);
		return NULL;
	}
	task->needs = parse_ids(member(map, "needs"), what, error);
	if (task->needs == NULL) {
		task_free(task);
		return NULL;
	}
	return task;
}

gint
bar_devservices_stack_find_service(const BarDevservicesStack *stack,
                                   const gchar *id)
{
	guint i;

	if (stack == NULL || id == NULL)
		return -1;
	for (i = 0; i < stack->services->len; i++) {
		BarDevservicesService *svc = g_ptr_array_index(stack->services, i);

		if (strcmp(svc->id, id) == 0)
			return (gint)i;
	}
	return -1;
}

BarDevservicesTask *
bar_devservices_stack_find_task(const BarDevservicesStack *stack,
                                const gchar *id)
{
	guint i;

	if (stack == NULL || id == NULL)
		return NULL;
	for (i = 0; i < stack->tasks->len; i++) {
		BarDevservicesTask *task = g_ptr_array_index(stack->tasks, i);

		if (strcmp(task->id, id) == 0)
			return task;
	}
	return NULL;
}

/*
 * check_refs:
 * @stack: a freshly parsed stack
 * @error: return location for a #GError
 *
 * Ids are unique and every `needs' names a service of this stack.
 * Cycles are left to bar_devservices_plan(), which has to walk the
 * graph anyway and is where the user's action fails.
 */
static gboolean
check_refs(BarDevservicesStack *stack, GError **error)
{
	guint i, j;

	for (i = 0; i < stack->services->len; i++) {
		BarDevservicesService *a = g_ptr_array_index(stack->services, i);

		for (j = i + 1; j < stack->services->len; j++) {
			BarDevservicesService *b;

			b = g_ptr_array_index(stack->services, j);
			if (strcmp(a->id, b->id) == 0) {
				g_set_error(error, BAR_DEVSERVICES_ERROR,
				            BAR_DEVSERVICES_ERROR_INVALID,
				            "service id `%s' is used twice", a->id);
				return FALSE;
			}
		}
		for (j = 0; a->needs[j] != NULL; j++) {
			if (bar_devservices_stack_find_service(stack,
			                                       a->needs[j]) < 0) {
				g_set_error(error, BAR_DEVSERVICES_ERROR,
				            BAR_DEVSERVICES_ERROR_INVALID,
				            "service `%s' needs `%s', which is not a "
				            "service", a->id, a->needs[j]);
				return FALSE;
			}
		}
	}
	for (i = 0; i < stack->tasks->len; i++) {
		BarDevservicesTask *t = g_ptr_array_index(stack->tasks, i);

		for (j = i + 1; j < stack->tasks->len; j++) {
			BarDevservicesTask *u = g_ptr_array_index(stack->tasks, j);

			if (strcmp(t->id, u->id) == 0) {
				g_set_error(error, BAR_DEVSERVICES_ERROR,
				            BAR_DEVSERVICES_ERROR_INVALID,
				            "task id `%s' is used twice", t->id);
				return FALSE;
			}
		}
		for (j = 0; t->needs[j] != NULL; j++) {
			if (bar_devservices_stack_find_service(stack,
			                                       t->needs[j]) < 0) {
				g_set_error(error, BAR_DEVSERVICES_ERROR,
				            BAR_DEVSERVICES_ERROR_INVALID,
				            "task `%s' needs `%s', which is not a "
				            "service", t->id, t->needs[j]);
				return FALSE;
			}
		}
	}
	return TRUE;
}

/*
 * parse_list:
 * @root_map: the document
 * @key: `services' or `tasks'
 * @n_out: (out): the entry count
 * @error: return location for a #GError
 *
 * Returns: (transfer none) (nullable): the list, or %NULL when absent
 *   (with *@error unset) or wrong (with it set)
 */
static YamlSequence *
parse_list(YamlMapping *root_map, const gchar *key, guint *n_out,
           GError **error)
{
	YamlNode *node;
	YamlSequence *seq;

	*n_out = 0;
	node = member(root_map, key);
	if (node == NULL || yaml_node_get_node_type(node) == YAML_NODE_NULL)
		return NULL;
	seq = (yaml_node_get_node_type(node) == YAML_NODE_SEQUENCE)
		? yaml_node_get_sequence(node) : NULL;
	if (seq == NULL) {
		g_set_error(error, BAR_DEVSERVICES_ERROR,
		            BAR_DEVSERVICES_ERROR_PARSE, "`%s' must be a list",
		            key);
		return NULL;
	}
	*n_out = yaml_sequence_get_length(seq);
	if (*n_out > 64) {
		g_set_error(error, BAR_DEVSERVICES_ERROR,
		            BAR_DEVSERVICES_ERROR_INVALID,
		            "`%s' has more than 64 entries", key);
		return NULL;
	}
	return seq;
}

BarDevservicesStack *
bar_devservices_stack_parse(const gchar *text, const gchar *key,
                            const gchar *file, GError **error)
{
	g_autoptr(YamlParser) parser = NULL;
	YamlNode *root;
	YamlMapping *map;
	YamlSequence *seq;
	BarDevservicesStack *stack;
	const gchar *value;
	GError *local = NULL;
	guint i, n;

	g_return_val_if_fail(text != NULL, NULL);

	if (!bar_devservices_valid_id(key)) {
		g_set_error(error, BAR_DEVSERVICES_ERROR,
		            BAR_DEVSERVICES_ERROR_INVALID,
		            "the file name must be letters, digits, `_', `.' "
		            "and `-' before .yaml");
		return NULL;
	}
	if (!g_utf8_validate(text, -1, NULL)) {
		g_set_error(error, BAR_DEVSERVICES_ERROR,
		            BAR_DEVSERVICES_ERROR_PARSE, "the file is not UTF-8");
		return NULL;
	}

	parser = yaml_parser_new();
	if (!yaml_parser_load_from_data(parser, text, -1, &local)) {
		g_set_error(error, BAR_DEVSERVICES_ERROR,
		            BAR_DEVSERVICES_ERROR_PARSE, "%s", local->message);
		g_error_free(local);
		return NULL;
	}
	root = yaml_parser_get_root(parser);
	map = (root != NULL &&
	       yaml_node_get_node_type(root) == YAML_NODE_MAPPING)
		? yaml_node_get_mapping(root) : NULL;
	if (map == NULL) {
		g_set_error(error, BAR_DEVSERVICES_ERROR,
		            BAR_DEVSERVICES_ERROR_PARSE,
		            "a stack must be a mapping with name, root and "
		            "services");
		return NULL;
	}

	value = scalar_member(map, "root");
	if (value == NULL || value[0] == '\0') {
		g_set_error(error, BAR_DEVSERVICES_ERROR,
		            BAR_DEVSERVICES_ERROR_INVALID,
		            "`root' (the project directory) is required");
		return NULL;
	}

	stack = g_new0(BarDevservicesStack, 1);
	stack->key = g_strdup(key);
	stack->file = g_strdup(file);
	stack->services = g_ptr_array_new_with_free_func(service_free);
	stack->tasks = g_ptr_array_new_with_free_func(task_free);
	{
		g_autofree gchar *expanded = expand_tilde(value);

		if (!g_path_is_absolute(expanded)) {
			g_set_error(error, BAR_DEVSERVICES_ERROR,
			            BAR_DEVSERVICES_ERROR_INVALID,
			            "`root' must be an absolute path or start "
			            "with ~/");
			bar_devservices_stack_free(stack);
			return NULL;
		}
		stack->root = g_canonicalize_filename(expanded, "/");
	}
	value = scalar_member(map, "name");
	stack->name = g_utf8_make_valid((value != NULL && value[0] != '\0')
	                                ? value : key, -1);

	seq = parse_list(map, "services", &n, &local);
	if (local != NULL) {
		g_propagate_error(error, local);
		bar_devservices_stack_free(stack);
		return NULL;
	}
	for (i = 0; i < n; i++) {
		YamlMapping *entry = yaml_sequence_get_mapping_element(seq, i);
		BarDevservicesService *svc;

		if (entry == NULL) {
			g_set_error(error, BAR_DEVSERVICES_ERROR,
			            BAR_DEVSERVICES_ERROR_PARSE,
			            "service %u is not a mapping", i + 1);
			bar_devservices_stack_free(stack);
			return NULL;
		}
		svc = parse_service(entry, stack->root, i, error);
		if (svc == NULL) {
			bar_devservices_stack_free(stack);
			return NULL;
		}
		g_ptr_array_add(stack->services, svc);
	}

	seq = parse_list(map, "tasks", &n, &local);
	if (local != NULL) {
		g_propagate_error(error, local);
		bar_devservices_stack_free(stack);
		return NULL;
	}
	for (i = 0; i < n; i++) {
		YamlMapping *entry = yaml_sequence_get_mapping_element(seq, i);
		BarDevservicesTask *task;

		if (entry == NULL) {
			g_set_error(error, BAR_DEVSERVICES_ERROR,
			            BAR_DEVSERVICES_ERROR_PARSE,
			            "task %u is not a mapping", i + 1);
			bar_devservices_stack_free(stack);
			return NULL;
		}
		task = parse_task(entry, stack->root, i, error);
		if (task == NULL) {
			bar_devservices_stack_free(stack);
			return NULL;
		}
		g_ptr_array_add(stack->tasks, task);
	}

	if (!check_refs(stack, error)) {
		bar_devservices_stack_free(stack);
		return NULL;
	}
	return stack;
}

/* Stack file names, sorted, so the panel order is stable. */
static GPtrArray *
stack_files(const gchar *dir)
{
	GPtrArray *names;
	GDir *d;
	const gchar *name;

	names = g_ptr_array_new_with_free_func(g_free);
	d = g_dir_open(dir, 0, NULL);
	if (d == NULL)
		return names;
	while ((name = g_dir_read_name(d)) != NULL && names->len < 128) {
		if (name[0] == '.')
			continue;
		if (g_str_has_suffix(name, ".yaml") ||
		    g_str_has_suffix(name, ".yml"))
			g_ptr_array_add(names, g_strdup(name));
	}
	g_dir_close(d);
	return names;
}

/* g_ptr_array_sort() hands the comparator pointers TO the elements. */
static gint
cmp_names(gconstpointer a, gconstpointer b)
{
	return strcmp(*(const gchar * const *)a, *(const gchar * const *)b);
}

GPtrArray *
bar_devservices_load_dir(const gchar *dir, GPtrArray *errors)
{
	GPtrArray *stacks;
	GPtrArray *names;
	guint i;

	stacks = g_ptr_array_new_with_free_func(
		(GDestroyNotify)bar_devservices_stack_free);
	if (dir == NULL)
		return stacks;
	names = stack_files(dir);
	g_ptr_array_sort(names, cmp_names);

	for (i = 0; i < names->len; i++) {
		const gchar *name = g_ptr_array_index(names, i);
		g_autofree gchar *path = g_build_filename(dir, name, NULL);
		g_autofree gchar *text = NULL;
		g_autofree gchar *key = NULL;
		g_autoptr(GError) error = NULL;
		BarDevservicesStack *stack;
		guint j;
		gboolean dup = FALSE;

		key = g_strndup(name, strlen(name) -
		                (g_str_has_suffix(name, ".yml") ? 4 : 5));
		/* `api.yaml' and `api.yml' would share a state file and
		   logs; the first one (by name) wins. */
		for (j = 0; j < stacks->len; j++) {
			BarDevservicesStack *other = g_ptr_array_index(stacks, j);

			if (strcmp(other->key, key) == 0)
				dup = TRUE;
		}
		if (dup) {
			if (errors != NULL)
				g_ptr_array_add(errors, g_strdup_printf(
					"%s: another file already defines `%s'",
					name, key));
			continue;
		}

		text = read_bounded(path, BAR_DEVSERVICES_MAX_FILE);
		if (text == NULL) {
			if (errors != NULL)
				g_ptr_array_add(errors, g_strdup_printf(
					"%s: cannot be read, or is larger than "
					"256 KiB", name));
			continue;
		}
		stack = bar_devservices_stack_parse(text, key, path, &error);
		if (stack == NULL) {
			if (errors != NULL)
				g_ptr_array_add(errors, g_strdup_printf("%s: %s",
					name, error->message));
			continue;
		}
		g_ptr_array_add(stacks, stack);
	}
	g_ptr_array_unref(names);
	return stacks;
}

gchar *
bar_devservices_dir_signature(const gchar *dir)
{
	GString *sig;
	GPtrArray *names;
	guint i;

	sig = g_string_new(NULL);
	if (dir == NULL)
		return g_string_free(sig, FALSE);
	names = stack_files(dir);
	g_ptr_array_sort(names, cmp_names);
	for (i = 0; i < names->len; i++) {
		const gchar *name = g_ptr_array_index(names, i);
		g_autofree gchar *path = g_build_filename(dir, name, NULL);
		GStatBuf st;

		if (g_stat(path, &st) != 0)
			continue;
		g_string_append_printf(sig, "%s:%" G_GINT64_FORMAT ":%"
		                       G_GINT64_FORMAT ";", name,
		                       (gint64)st.st_mtime, (gint64)st.st_size);
	}
	g_ptr_array_unref(names);
	return g_string_free(sig, FALSE);
}

/* ----------------------------------------------------------------
 * Ordering
 * ---------------------------------------------------------------- */

/*
 * The key a service sorts by: twice its `needs' depth, plus one for a
 * process.  With no `needs' anywhere that is 0 for every container and
 * 1 for every process, which is the simple "containers first" order;
 * with them, a process that needs a process still comes after it.
 */
GPtrArray *
bar_devservices_plan(const BarDevservicesStack *stack,
                     const gchar * const *only, GError **error)
{
	guint n, i, j, done, pass;
	gint *depth;
	gboolean *want;
	GPtrArray *waves;
	gint max_key = -1, key;

	g_return_val_if_fail(stack != NULL, NULL);

	n = stack->services->len;
	depth = g_new(gint, MAX(n, 1));
	want = g_new0(gboolean, MAX(n, 1));
	for (i = 0; i < n; i++)
		depth[i] = -1;

	/* The closure of what was asked for. */
	if (only == NULL) {
		for (i = 0; i < n; i++)
			want[i] = TRUE;
	} else {
		gboolean grew = TRUE;

		for (i = 0; only[i] != NULL; i++) {
			gint idx = bar_devservices_stack_find_service(stack,
			                                              only[i]);

			if (idx >= 0)
				want[idx] = TRUE;
		}
		while (grew) {
			grew = FALSE;
			for (i = 0; i < n; i++) {
				BarDevservicesService *svc;

				if (!want[i])
					continue;
				svc = g_ptr_array_index(stack->services, i);
				for (j = 0; svc->needs[j] != NULL; j++) {
					gint idx = bar_devservices_stack_find_service(
						stack, svc->needs[j]);

					if (idx >= 0 && !want[idx]) {
						want[idx] = TRUE;
						grew = TRUE;
					}
				}
			}
		}
	}

	/*
	 * Depths by relaxation: a service's depth is known once all its
	 * needs' are.  n passes are enough for any acyclic graph; a pass
	 * that settles nothing while services remain means a cycle.
	 */
	done = 0;
	for (pass = 0; pass <= n && done < n; pass++) {
		guint settled = 0;

		for (i = 0; i < n; i++) {
			BarDevservicesService *svc;
			gint d = 0;
			gboolean ready = TRUE;

			if (depth[i] >= 0)
				continue;
			svc = g_ptr_array_index(stack->services, i);
			for (j = 0; svc->needs[j] != NULL; j++) {
				gint idx = bar_devservices_stack_find_service(
					stack, svc->needs[j]);

				if (idx < 0)
					continue;
				if (depth[idx] < 0) {
					ready = FALSE;
					break;
				}
				d = MAX(d, depth[idx] + 1);
			}
			if (ready) {
				depth[i] = d;
				settled++;
				done++;
			}
		}
		if (settled == 0)
			break;
	}
	if (done < n) {
		GString *names = g_string_new(NULL);

		for (i = 0; i < n; i++) {
			if (depth[i] < 0) {
				BarDevservicesService *svc;

				svc = g_ptr_array_index(stack->services, i);
				g_string_append_printf(names, "%s%s",
				                       names->len > 0 ? ", " : "",
				                       svc->id);
			}
		}
		g_set_error(error, BAR_DEVSERVICES_ERROR,
		            BAR_DEVSERVICES_ERROR_CYCLE,
		            "`needs' goes round in a circle through %s",
		            names->str);
		g_string_free(names, TRUE);
		g_free(depth);
		g_free(want);
		return NULL;
	}

	for (i = 0; i < n; i++) {
		BarDevservicesService *svc = g_ptr_array_index(stack->services, i);

		if (!want[i])
			continue;
		key = depth[i] * 2 +
		      (svc->kind == BAR_DEVSERVICES_KIND_PROCESS ? 1 : 0);
		max_key = MAX(max_key, key);
	}

	waves = g_ptr_array_new_with_free_func((GDestroyNotify)g_array_unref);
	for (key = 0; key <= max_key; key++) {
		GArray *wave = g_array_new(FALSE, FALSE, sizeof(guint));

		for (i = 0; i < n; i++) {
			BarDevservicesService *svc;
			gint k;

			if (!want[i])
				continue;
			svc = g_ptr_array_index(stack->services, i);
			k = depth[i] * 2 +
			    (svc->kind == BAR_DEVSERVICES_KIND_PROCESS ? 1 : 0);
			if (k == key)
				g_array_append_val(wave, i);
		}
		if (wave->len > 0)
			g_ptr_array_add(waves, wave);
		else
			g_array_unref(wave);
	}
	g_free(depth);
	g_free(want);
	return waves;
}

/* ----------------------------------------------------------------
 * Locating the project
 * ---------------------------------------------------------------- */

gsize
bar_devservices_path_within(const gchar *root, const gchar *path)
{
	g_autofree gchar *r = NULL;
	g_autofree gchar *p = NULL;
	gsize len;

	if (root == NULL || path == NULL)
		return 0;
	r = resolve(root);
	p = resolve(path);
	len = strlen(r);
	/* `/' contains everything, and is always the wrong answer for
	   "which project is this". */
	if (len <= 1)
		return 0;
	if (strncmp(r, p, len) != 0)
		return 0;
	if (p[len] != '\0' && p[len] != '/')
		return 0;
	return len;
}

gint
bar_devservices_match_root(GPtrArray *stacks, const gchar *cwd)
{
	guint i;
	gsize best_len = 0;
	gint best = -1;

	if (stacks == NULL || cwd == NULL || cwd[0] == '\0')
		return -1;
	for (i = 0; i < stacks->len; i++) {
		BarDevservicesStack *stack = g_ptr_array_index(stacks, i);
		gsize len = bar_devservices_path_within(stack->root, cwd);

		if (len > best_len) {
			best_len = len;
			best = (gint)i;
		}
	}
	return best;
}

gboolean
bar_devservices_parse_stat(const gchar *text, gint *ppid, gint *pgrp,
                           guint64 *starttime, gchar **comm, gchar *state)
{
	const gchar *open, *close;
	g_auto(GStrv) fields = NULL;

	if (text == NULL)
		return FALSE;
	open = strchr(text, '(');
	close = strrchr(text, ')');
	if (open == NULL || close == NULL || close < open)
		return FALSE;

	/* After `) ': state(0) ppid(1) pgrp(2) ... starttime(19). */
	fields = g_strsplit_set(close + 1, " \n", -1);
	{
		GPtrArray *f = g_ptr_array_new();
		guint i;

		for (i = 0; fields[i] != NULL; i++)
			if (fields[i][0] != '\0')
				g_ptr_array_add(f, fields[i]);
		if (f->len < 20) {
			g_ptr_array_free(f, TRUE);
			return FALSE;
		}
		if (state != NULL)
			*state = ((const gchar *)g_ptr_array_index(f, 0))[0];
		if (ppid != NULL)
			*ppid = (gint)g_ascii_strtoll(g_ptr_array_index(f, 1),
			                              NULL, 10);
		if (pgrp != NULL)
			*pgrp = (gint)g_ascii_strtoll(g_ptr_array_index(f, 2),
			                              NULL, 10);
		if (starttime != NULL)
			*starttime = g_ascii_strtoull(g_ptr_array_index(f, 19),
			                              NULL, 10);
		g_ptr_array_free(f, TRUE);
	}
	if (comm != NULL)
		*comm = g_strndup(open + 1, (gsize)(close - open - 1));
	return TRUE;
}

/* One process as /proc describes it. */
typedef struct {
	gint    pid;
	gint    ppid;
	gint    pgrp;
	guint64 starttime;
	gchar   state;
	gchar  *comm;
} ProcEntry;

static void
proc_entry_clear(gpointer data)
{
	g_free(((ProcEntry *)data)->comm);
}

/*
 * proc_snapshot:
 * @proc_root: normally `/proc'
 *
 * Every process's stat line, parsed.  Bounded at 65536 entries, which
 * is far beyond a workstation and keeps a pathological /proc from
 * running away with a worker.
 *
 * Returns: (transfer full) (element-type ProcEntry): the processes
 */
static GArray *
proc_snapshot(const gchar *proc_root)
{
	GArray *out;
	GDir *d;
	const gchar *name;

	out = g_array_new(FALSE, TRUE, sizeof(ProcEntry));
	g_array_set_clear_func(out, proc_entry_clear);
	d = g_dir_open(proc_root, 0, NULL);
	if (d == NULL)
		return out;
	while ((name = g_dir_read_name(d)) != NULL && out->len < 65536) {
		g_autofree gchar *path = NULL;
		g_autofree gchar *text = NULL;
		ProcEntry e;

		if (!g_ascii_isdigit(name[0]))
			continue;
		memset(&e, 0, sizeof(e));
		e.pid = (gint)g_ascii_strtoll(name, NULL, 10);
		path = g_build_filename(proc_root, name, "stat", NULL);
		if (!g_file_get_contents(path, &text, NULL, NULL))
			continue;
		if (!bar_devservices_parse_stat(text, &e.ppid, &e.pgrp,
		                                &e.starttime, &e.comm, &e.state))
			continue;
		g_array_append_val(out, e);
	}
	g_dir_close(d);
	return out;
}

guint64
bar_devservices_proc_starttime(const gchar *proc_root, gint pid)
{
	g_autofree gchar *path = NULL;
	g_autofree gchar *text = NULL;
	gchar pid_s[32];
	guint64 start = 0;
	gchar state = 0;

	if (pid <= 0)
		return 0;
	g_snprintf(pid_s, sizeof(pid_s), "%d", pid);
	path = g_build_filename(proc_root, pid_s, "stat", NULL);
	if (!g_file_get_contents(path, &text, NULL, NULL))
		return 0;
	if (!bar_devservices_parse_stat(text, NULL, NULL, &start, NULL, &state))
		return 0;
	/* A zombie has exited; only its parent's wait is left. */
	if (state == 'Z' || state == 'X')
		return 0;
	return start;
}

/* The shells a terminal can be showing. */
static gboolean
is_shell(const gchar *comm)
{
	static const gchar *const shells[] = {
		"bash", "zsh", "fish", "sh", "dash", "ksh", "mksh", "tcsh",
		"csh", "nu", "elvish", "xonsh", "ash", "yash", "bacon", NULL
	};
	guint i;

	if (comm == NULL)
		return FALSE;
	/* A login shell's comm starts with `-'. */
	if (comm[0] == '-')
		comm++;
	for (i = 0; shells[i] != NULL; i++)
		if (strcmp(comm, shells[i]) == 0)
			return TRUE;
	return FALSE;
}

static gchar *
read_cwd(const gchar *proc_root, gint pid)
{
	gchar pid_s[32];
	g_autofree gchar *link = NULL;

	g_snprintf(pid_s, sizeof(pid_s), "%d", pid);
	link = g_build_filename(proc_root, pid_s, "cwd", NULL);
	return g_file_read_link(link, NULL);
}

gchar *
bar_devservices_focus_cwd(const gchar *proc_root, gint pid)
{
	g_autoptr(GArray) procs = NULL;
	GArray *frontier, *next;
	guint i, depth = 0;
	gint best = 0;
	guint best_depth = 0;
	guint64 best_start = 0;

	if (pid <= 0)
		return NULL;
	procs = proc_snapshot(proc_root);

	/* Breadth first: each level is the previous level's children.
	   Depth is capped so a ppid loop in a fabricated (or racing)
	   /proc cannot spin. */
	frontier = g_array_new(FALSE, FALSE, sizeof(gint));
	g_array_append_val(frontier, pid);
	while (frontier->len > 0 && depth < 64) {
		next = g_array_new(FALSE, FALSE, sizeof(gint));
		depth++;
		for (i = 0; i < procs->len; i++) {
			ProcEntry *e = &g_array_index(procs, ProcEntry, i);
			guint k;

			for (k = 0; k < frontier->len; k++) {
				if (e->ppid != g_array_index(frontier, gint, k) ||
				    e->pid == pid)
					continue;
				g_array_append_val(next, e->pid);
				if (is_shell(e->comm) && e->state != 'Z' &&
				    (depth > best_depth ||
				     (depth == best_depth &&
				      e->starttime > best_start))) {
					best = e->pid;
					best_depth = depth;
					best_start = e->starttime;
				}
				break;
			}
		}
		g_array_unref(frontier);
		frontier = next;
	}
	g_array_unref(frontier);

	if (best > 0) {
		gchar *cwd = read_cwd(proc_root, best);

		if (cwd != NULL)
			return cwd;
	}
	return read_cwd(proc_root, pid);
}

/* ----------------------------------------------------------------
 * Ports
 * ---------------------------------------------------------------- */

guint
bar_devservices_parse_net_tcp(const gchar *text, GHashTable *into)
{
	g_auto(GStrv) lines = NULL;
	guint i, found = 0;

	if (text == NULL || into == NULL)
		return 0;
	lines = g_strsplit(text, "\n", -1);
	/* Line 0 is the column header -- and an empty file has no line 0,
	   so starting at 1 would read past the vector's terminator. */
	if (lines[0] == NULL)
		return 0;
	for (i = 1; lines[i] != NULL; i++) {
		g_auto(GStrv) f = NULL;
		const gchar *local, *colon;
		gchar *end = NULL;
		guint64 port, inode;
		guint k, n = 0;
		const gchar *col[10];

		f = g_strsplit_set(lines[i], " \t", -1);
		for (k = 0; f[k] != NULL && n < G_N_ELEMENTS(col); k++)
			if (f[k][0] != '\0')
				col[n++] = f[k];
		if (n < 10)
			continue;
		/* st == 0A is TCP_LISTEN. */
		if (g_ascii_strcasecmp(col[3], "0A") != 0)
			continue;
		local = col[1];
		colon = strrchr(local, ':');
		if (colon == NULL)
			continue;
		port = g_ascii_strtoull(colon + 1, &end, 16);
		if (end == colon + 1 || *end != '\0' || port == 0 ||
		    port > 65535)
			continue;
		inode = g_ascii_strtoull(col[9], &end, 10);
		if (*end != '\0')
			continue;
		g_hash_table_insert(into, GUINT_TO_POINTER((guint)port),
		                    g_memdup2(&inode, sizeof(inode)));
		found++;
	}
	return found;
}

GHashTable *
bar_devservices_listening(const gchar *proc_root)
{
	GHashTable *ports;
	static const gchar *const files[] = { "tcp", "tcp6", NULL };
	guint i;

	ports = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL,
	                              g_free);
	for (i = 0; files[i] != NULL; i++) {
		g_autofree gchar *path = NULL;
		g_autofree gchar *text = NULL;

		path = g_build_filename(proc_root, "net", files[i], NULL);
		/* /proc files report a size of 0, so there is nothing to
		   bound by up front; the kernel generates one line per
		   socket and this only ever runs on a worker. */
		if (!g_file_get_contents(path, &text, NULL, NULL))
			continue;
		bar_devservices_parse_net_tcp(text, ports);
	}
	return ports;
}

gint
bar_devservices_socket_owner(const gchar *proc_root, guint64 inode,
                             uid_t uid)
{
	GDir *d;
	const gchar *name;
	gchar want[64];
	gint owner = 0;

	if (inode == 0)
		return 0;
	g_snprintf(want, sizeof(want), "socket:[%" G_GUINT64_FORMAT "]", inode);
	d = g_dir_open(proc_root, 0, NULL);
	if (d == NULL)
		return 0;
	while (owner == 0 && (name = g_dir_read_name(d)) != NULL) {
		g_autofree gchar *pdir = NULL;
		g_autofree gchar *fddir = NULL;
		GStatBuf st;
		GDir *fds;
		const gchar *fd;

		if (!g_ascii_isdigit(name[0]))
			continue;
		pdir = g_build_filename(proc_root, name, NULL);
		/* Only our own processes: another user's server is not
		   ours to take a port from, and its fds are not readable
		   anyway. */
		if (g_stat(pdir, &st) != 0 || st.st_uid != uid)
			continue;
		fddir = g_build_filename(pdir, "fd", NULL);
		fds = g_dir_open(fddir, 0, NULL);
		if (fds == NULL)
			continue;
		while ((fd = g_dir_read_name(fds)) != NULL) {
			g_autofree gchar *link = NULL;
			g_autofree gchar *target = NULL;

			link = g_build_filename(fddir, fd, NULL);
			target = g_file_read_link(link, NULL);
			if (target != NULL && strcmp(target, want) == 0) {
				owner = (gint)g_ascii_strtoll(name, NULL, 10);
				break;
			}
		}
		g_dir_close(fds);
	}
	g_dir_close(d);
	return owner;
}

/*
 * connect_one:
 * @addr: a sockaddr
 * @len: its length
 * @timeout_ms: how long to wait
 *
 * A non-blocking connect with a deadline: a blocking one to a port a
 * firewall drops waits the kernel's SYN timeout, minutes.
 */
static gboolean
connect_one(const struct sockaddr *addr, socklen_t len, gint timeout_ms)
{
	gint fd, rc, err = 0;
	socklen_t elen = sizeof(err);
	struct pollfd pfd;

	fd = socket(addr->sa_family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
	            0);
	if (fd < 0)
		return FALSE;
	rc = connect(fd, addr, len);
	if (rc == 0) {
		close(fd);
		return TRUE;
	}
	if (errno != EINPROGRESS) {
		close(fd);
		return FALSE;
	}
	pfd.fd = fd;
	pfd.events = POLLOUT;
	pfd.revents = 0;
	rc = poll(&pfd, 1, timeout_ms);
	if (rc <= 0 ||
	    getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &elen) != 0 ||
	    err != 0) {
		close(fd);
		return FALSE;
	}
	close(fd);
	return TRUE;
}

gboolean
bar_devservices_port_connects(gint port, gint timeout_ms)
{
	struct sockaddr_in in4;
	struct sockaddr_in6 in6;

	if (port <= 0 || port > 65535)
		return FALSE;
	memset(&in4, 0, sizeof(in4));
	in4.sin_family = AF_INET;
	in4.sin_port = htons((guint16)port);
	in4.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (connect_one((const struct sockaddr *)&in4, sizeof(in4), timeout_ms))
		return TRUE;
	memset(&in6, 0, sizeof(in6));
	in6.sin6_family = AF_INET6;
	in6.sin6_port = htons((guint16)port);
	in6.sin6_addr = in6addr_loopback;
	return connect_one((const struct sockaddr *)&in6, sizeof(in6),
	                   timeout_ms);
}

/* ----------------------------------------------------------------
 * Runs and the state file
 * ---------------------------------------------------------------- */

BarDevservicesRun *
bar_devservices_run_new(const gchar *id)
{
	BarDevservicesRun *run = g_new0(BarDevservicesRun, 1);

	run->id = g_strdup(id);
	return run;
}

void
bar_devservices_run_free(BarDevservicesRun *run)
{
	if (run == NULL)
		return;
	g_free(run->id);
	g_free(run);
}

gchar *
bar_devservices_state_to_text(GPtrArray *runs)
{
	GString *out;
	guint i;

	out = g_string_new("# gowl devservices: processes this widget "
	                   "started, so a restarted gowl\n# can still see "
	                   "and stop them.  Written by the bar; edits are "
	                   "lost.\n");
	for (i = 0; runs != NULL && i < runs->len; i++) {
		BarDevservicesRun *run = g_ptr_array_index(runs, i);

		if (!valid_run_id(run->id) || run->pid <= 0)
			continue;
		g_string_append_printf(out, "%s.pid=%d\n", run->id, run->pid);
		g_string_append_printf(out, "%s.pgid=%d\n", run->id, run->pgid);
		g_string_append_printf(out, "%s.starttime=%" G_GUINT64_FORMAT
		                       "\n", run->id, run->starttime);
		g_string_append_printf(out, "%s.started=%" G_GINT64_FORMAT "\n",
		                       run->id, run->started);
	}
	return g_string_free(out, FALSE);
}

static BarDevservicesRun *
find_run(GPtrArray *runs, const gchar *id)
{
	guint i;

	for (i = 0; i < runs->len; i++) {
		BarDevservicesRun *run = g_ptr_array_index(runs, i);

		if (strcmp(run->id, id) == 0)
			return run;
	}
	return NULL;
}

GPtrArray *
bar_devservices_state_from_text(const gchar *text)
{
	GPtrArray *runs, *out;
	g_auto(GStrv) lines = NULL;
	guint i;

	runs = g_ptr_array_new_with_free_func(
		(GDestroyNotify)bar_devservices_run_free);
	if (text == NULL)
		return runs;
	lines = g_strsplit(text, "\n", 4096);
	for (i = 0; lines[i] != NULL; i++) {
		g_autofree gchar *id = NULL;
		const gchar *eq, *dot, *value;
		gchar *end = NULL;
		gint64 v;
		BarDevservicesRun *run;

		if (lines[i][0] == '#' || lines[i][0] == '\0')
			continue;
		eq = strchr(lines[i], '=');
		if (eq == NULL)
			continue;
		/* Ids may contain `.', keys never do: split at the last
		   dot before the `='. */
		dot = g_strrstr_len(lines[i], eq - lines[i], ".");
		if (dot == NULL)
			continue;
		id = g_strndup(lines[i], (gsize)(dot - lines[i]));
		if (!valid_run_id(id))
			continue;
		value = eq + 1;
		v = g_ascii_strtoll(value, &end, 10);
		if (end == value || *end != '\0' || v < 0)
			continue;

		run = find_run(runs, id);
		if (run == NULL) {
			run = bar_devservices_run_new(id);
			g_ptr_array_add(runs, run);
		}
		if (strncmp(dot + 1, "pid=", 4) == 0)
			run->pid = (gint)MIN(v, G_MAXINT);
		else if (strncmp(dot + 1, "pgid=", 5) == 0)
			run->pgid = (gint)MIN(v, G_MAXINT);
		else if (strncmp(dot + 1, "starttime=", 10) == 0)
			run->starttime = g_ascii_strtoull(value, NULL, 10);
		else if (strncmp(dot + 1, "started=", 8) == 0)
			run->started = v;
	}

	/* A run the widget might signal must be complete: a pid with no
	   start time cannot be told from a reused one, and a pgid of 0 or
	   1 would signal our own group or every process we may. */
	out = g_ptr_array_new_with_free_func(
		(GDestroyNotify)bar_devservices_run_free);
	while (runs->len > 0) {
		BarDevservicesRun *run = g_ptr_array_steal_index(runs, 0);

		if (run->pid > 1 && run->pgid > 1 && run->starttime > 0)
			g_ptr_array_add(out, run);
		else
			bar_devservices_run_free(run);
	}
	g_ptr_array_unref(runs);
	return out;
}

gboolean
bar_devservices_state_save(const gchar *path, GPtrArray *runs, GError **error)
{
	g_autofree gchar *text = NULL;
	g_autofree gchar *dir = NULL;

	if (runs == NULL || runs->len == 0) {
		if (g_unlink(path) != 0 && errno != ENOENT) {
			g_set_error(error, BAR_DEVSERVICES_ERROR,
			            BAR_DEVSERVICES_ERROR_IO,
			            "cannot remove %s: %s", path, g_strerror(errno));
			return FALSE;
		}
		return TRUE;
	}
	dir = g_path_get_dirname(path);
	if (g_mkdir_with_parents(dir, 0700) != 0) {
		g_set_error(error, BAR_DEVSERVICES_ERROR,
		            BAR_DEVSERVICES_ERROR_IO, "cannot create %s: %s",
		            dir, g_strerror(errno));
		return FALSE;
	}
	text = bar_devservices_state_to_text(runs);
	return g_file_set_contents_full(path, text, -1,
	                                G_FILE_SET_CONTENTS_CONSISTENT, 0600,
	                                error);
}

GPtrArray *
bar_devservices_state_load(const gchar *path)
{
	g_autofree gchar *text = NULL;

	text = read_bounded(path, 64 * 1024);
	return bar_devservices_state_from_text(text);
}

BarDevservicesLiveness
bar_devservices_run_liveness(const gchar *proc_root,
                             const BarDevservicesRun *run)
{
	guint64 now;
	g_autoptr(GArray) procs = NULL;
	guint i;
	gboolean members = FALSE;

	if (run == NULL || run->pid <= 1 || run->pgid <= 1)
		return BAR_DEVSERVICES_GONE;

	now = bar_devservices_proc_starttime(proc_root, run->pid);
	if (now != 0)
		return (now == run->starttime) ? BAR_DEVSERVICES_ALIVE
		                               : BAR_DEVSERVICES_MISMATCH;

	/*
	 * The leader is gone.  Its group can outlive it (a shell script
	 * that backgrounded the server and exited), and while any member
	 * lives the kernel does not hand that group id out again -- so a
	 * group with members all started after our leader is still ours.
	 * One that predates it cannot be.
	 */
	procs = proc_snapshot(proc_root);
	for (i = 0; i < procs->len; i++) {
		ProcEntry *e = &g_array_index(procs, ProcEntry, i);

		if (e->pgrp != run->pgid || e->state == 'Z' || e->state == 'X')
			continue;
		if (e->starttime < run->starttime)
			return BAR_DEVSERVICES_MISMATCH;
		members = TRUE;
	}
	return members ? BAR_DEVSERVICES_ORPHANS : BAR_DEVSERVICES_GONE;
}

BarDevservicesLiveness
bar_devservices_run_signal(const gchar *proc_root,
                           const BarDevservicesRun *run, gint sig)
{
	BarDevservicesLiveness live;

	live = bar_devservices_run_liveness(proc_root, run);
	if (live != BAR_DEVSERVICES_ALIVE && live != BAR_DEVSERVICES_ORPHANS)
		return live;
	/* Never our own group: setsid failing would leave the child in
	   it, and signalling it would take gowl down with the server. */
	if (run->pgid <= 1 || run->pgid == (gint)getpgrp())
		return BAR_DEVSERVICES_MISMATCH;
	if (kill(-(pid_t)run->pgid, sig) != 0 && errno != ESRCH)
		g_warning("gowl-bar: devservices: cannot signal group %d: %s",
		          run->pgid, g_strerror(errno));
	return live;
}

gboolean
bar_devservices_reap(gint pid, gint *status)
{
	gint st = 0;

	if (pid <= 0)
		return FALSE;
	if (waitpid((pid_t)pid, &st, WNOHANG) == (pid_t)pid) {
		if (status != NULL)
			*status = st;
		return TRUE;
	}
	return FALSE;
}

gboolean
bar_devservices_log_rotate(const gchar *path, goffset max)
{
	GStatBuf st;
	g_autofree gchar *old = NULL;
	g_autofree gchar *tmp = NULL;
	gint in, out;
	gchar buf[65536];
	gssize n;
	gboolean ok = TRUE;

	if (g_stat(path, &st) != 0 || (goffset)st.st_size <= max)
		return FALSE;
	old = g_strconcat(path, ".1", NULL);
	tmp = g_strconcat(path, ".1.tmp", NULL);
	in = g_open(path, O_RDONLY | O_CLOEXEC, 0);
	if (in < 0)
		return FALSE;
	out = g_open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
	if (out < 0) {
		close(in);
		return FALSE;
	}
	while (ok && (n = read(in, buf, sizeof(buf))) > 0) {
		gssize off = 0;

		while (off < n) {
			gssize w = write(out, buf + off, (gsize)(n - off));

			if (w <= 0) {
				ok = FALSE;
				break;
			}
			off += w;
		}
	}
	close(in);
	close(out);
	if (!ok || g_rename(tmp, old) != 0) {
		g_unlink(tmp);
		return FALSE;
	}
	/* In place: the writer's O_APPEND descriptor stays valid. */
	if (truncate(path, 0) != 0)
		return FALSE;
	return TRUE;
}

gchar *
bar_devservices_log_tail(const gchar *path)
{
	gint fd;
	struct stat st;
	gchar buf[4097];
	gssize n;
	off_t from;
	g_auto(GStrv) lines = NULL;
	gint i;

	fd = g_open(path, O_RDONLY | O_CLOEXEC, 0);
	if (fd < 0)
		return NULL;
	if (fstat(fd, &st) != 0) {
		close(fd);
		return NULL;
	}
	from = (st.st_size > 4096) ? st.st_size - 4096 : 0;
	if (lseek(fd, from, SEEK_SET) < 0) {
		close(fd);
		return NULL;
	}
	n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (n <= 0)
		return NULL;
	buf[n] = '\0';
	/* NULs from a binary log would end the string early. */
	for (i = 0; i < n; i++)
		if (buf[i] == '\0')
			buf[i] = ' ';

	lines = g_strsplit_set(buf, "\r\n", -1);
	for (i = (gint)g_strv_length(lines) - 1; i >= 0; i--) {
		g_autofree gchar *valid = NULL;
		gchar *line;

		/* The first line of a window that started mid-file is a
		   fragment; skip it unless it is all there is. */
		if (i == 0 && from > 0)
			break;
		line = lines[i];
		bar_strip_ansi(line);
		g_strstrip(line);
		if (line[0] == '\0')
			continue;
		valid = g_utf8_make_valid(line, -1);
		if (g_utf8_strlen(valid, -1) > 120)
			return g_utf8_substring(valid, 0, 120);
		return g_steal_pointer(&valid);
	}
	return NULL;
}

/* Runs in the child between fork and exec. */
static void
child_new_session(gpointer data)
{
	(void)data;
	/* A new session is a new process group led by the child: stopping
	   it is then kill(-pgid), which reaches every process the
	   service starts and nothing gowl owns.  It also drops the
	   controlling terminal, so a server cannot grab gowl's tty. */
	setsid();
}

BarDevservicesRun *
bar_devservices_spawn(const gchar * const *argv, const gchar *dir,
                      const gchar * const *env, const gchar *log_path,
                      const gchar *id, const gchar *proc_root, GError **error)
{
	g_auto(GStrv) envp = NULL;
	g_autofree gchar *log_dir = NULL;
	BarDevservicesRun *run;
	GPid pid = 0;
	gint log_fd, null_fd;
	guint i;
	gboolean ok;

	g_return_val_if_fail(argv != NULL && argv[0] != NULL, NULL);

	if (dir != NULL && !g_file_test(dir, G_FILE_TEST_IS_DIR)) {
		g_set_error(error, BAR_DEVSERVICES_ERROR,
		            BAR_DEVSERVICES_ERROR_SPAWN,
		            "the directory %s does not exist", dir);
		return NULL;
	}

	log_dir = g_path_get_dirname(log_path);
	g_mkdir_with_parents(log_dir, 0700);
	bar_devservices_log_rotate(log_path, BAR_DEVSERVICES_LOG_MAX);
	/* 0600: a dev server's log carries tokens and connection strings
	   often enough that nobody else on the machine should read it. */
	log_fd = g_open(log_path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC,
	                0600);
	if (log_fd < 0) {
		g_set_error(error, BAR_DEVSERVICES_ERROR,
		            BAR_DEVSERVICES_ERROR_IO, "cannot open %s: %s",
		            log_path, g_strerror(errno));
		return NULL;
	}
	null_fd = g_open("/dev/null", O_RDONLY | O_CLOEXEC, 0);

	envp = g_get_environ();
	for (i = 0; env != NULL && env[i] != NULL; i++) {
		g_auto(GStrv) kv = g_strsplit(env[i], "=", 2);

		if (kv[0] != NULL && kv[1] != NULL)
			envp = g_environ_setenv(envp, kv[0], kv[1], TRUE);
	}

	ok = g_spawn_async_with_fds(dir, (gchar **)argv, envp,
	                            G_SPAWN_SEARCH_PATH |
	                            G_SPAWN_DO_NOT_REAP_CHILD,
	                            child_new_session, NULL, &pid,
	                            null_fd, log_fd, log_fd, error);
	close(log_fd);
	if (null_fd >= 0)
		close(null_fd);
	if (!ok)
		return NULL;

	run = bar_devservices_run_new(id);
	run->pid = (gint)pid;
	run->pgid = (gint)pid;
	run->starttime = bar_devservices_proc_starttime(proc_root, (gint)pid);
	run->started = g_get_real_time() / G_USEC_PER_SEC;
	if (run->starttime == 0) {
		/* It exited before we could look.  Reap it now so it is
		   not left a zombie; the caller sees an immediate exit. */
		bar_devservices_reap(run->pid, NULL);
	}
	return run;
}

/* ----------------------------------------------------------------
 * Podman
 * ---------------------------------------------------------------- */

gchar **
bar_devservices_compose_argv(const gchar *compose_command,
                             const BarDevservicesService *svc, gboolean up)
{
	g_auto(GStrv) base = NULL;
	GPtrArray *argv;
	guint i;

	if (svc == NULL || svc->compose == NULL || svc->compose_service == NULL)
		return NULL;
	if (!g_shell_parse_argv((compose_command != NULL &&
	                         compose_command[0] != '\0')
	                        ? compose_command : "podman compose",
	                        NULL, &base, NULL))
		return NULL;

	argv = g_ptr_array_new_with_free_func(g_free);
	for (i = 0; base[i] != NULL; i++)
		g_ptr_array_add(argv, g_strdup(base[i]));
	g_ptr_array_add(argv, g_strdup("-f"));
	g_ptr_array_add(argv, g_strdup(svc->compose));
	if (svc->compose_project != NULL) {
		g_ptr_array_add(argv, g_strdup("-p"));
		g_ptr_array_add(argv, g_strdup(svc->compose_project));
	}
	if (up) {
		g_ptr_array_add(argv, g_strdup("up"));
		g_ptr_array_add(argv, g_strdup("-d"));
	} else {
		g_ptr_array_add(argv, g_strdup("down"));
	}
	g_ptr_array_add(argv, g_strdup(svc->compose_service));
	g_ptr_array_add(argv, NULL);
	g_ptr_array_set_free_func(argv, NULL);
	return (gchar **)g_ptr_array_free(argv, FALSE);
}

typedef struct {
	const BarDevservicesService *svc;
	gchar   *compose_dir;
	gchar   *default_project;
	gboolean running;
} PodmanMatch;

static gboolean
podman_container(const gchar *object, guint index, gpointer user_data)
{
	PodmanMatch *m = user_data;
	g_autofree gchar *state = NULL;
	g_autofree gchar *labels = NULL;
	g_autofree gchar *service = NULL;
	g_autofree gchar *project = NULL;
	g_autofree gchar *wd = NULL;
	gboolean same_project;

	(void)index;
	state = gowl_bar_json_string(object, "State");
	if (g_strcmp0(state, "running") != 0)
		return TRUE;
	labels = gowl_bar_json_object(object, "Labels");
	if (labels == NULL)
		return TRUE;
	service = gowl_bar_json_string(labels, "com.docker.compose.service");
	if (g_strcmp0(service, m->svc->compose_service) != 0)
		return TRUE;

	project = gowl_bar_json_string(labels, "com.docker.compose.project");
	if (project == NULL)
		project = gowl_bar_json_string(labels,
		                               "io.podman.compose.project");
	wd = gowl_bar_json_string(labels,
	                          "com.docker.compose.project.working_dir");
	if (m->svc->compose_project != NULL)
		same_project = (g_strcmp0(project, m->svc->compose_project) == 0);
	else
		same_project = (wd != NULL &&
		                bar_devservices_path_within(m->compose_dir, wd) &&
		                bar_devservices_path_within(wd, m->compose_dir)) ||
		               g_strcmp0(project, m->default_project) == 0;
	if (same_project) {
		m->running = TRUE;
		return FALSE;
	}
	return TRUE;
}

gboolean
bar_devservices_podman_running(const gchar *ps_json,
                               const BarDevservicesService *svc)
{
	PodmanMatch m;
	g_autofree gchar *wrapped = NULL;
	g_autofree gchar *base = NULL;

	if (ps_json == NULL || svc == NULL || svc->compose == NULL)
		return FALSE;
	memset(&m, 0, sizeof(m));
	m.svc = svc;
	m.compose_dir = g_path_get_dirname(svc->compose);
	base = g_path_get_basename(m.compose_dir);
	/* Compose's default project name: the directory, lower-cased,
	   with what it cannot use dropped. */
	{
		GString *p = g_string_new(NULL);
		const gchar *c;

		for (c = base; *c != '\0'; c++) {
			gchar l = g_ascii_tolower(*c);

			if (g_ascii_isalnum(l) || l == '_' || l == '-')
				g_string_append_c(p, l);
		}
		m.default_project = g_string_free(p, FALSE);
	}
	/* `podman ps --format json' is a bare array; the JSON reader
	   walks the value of a key. */
	wrapped = g_strdup_printf("{\"c\":%s}", ps_json);
	gowl_bar_json_foreach_object(wrapped, "c", podman_container, &m);
	g_free(m.compose_dir);
	g_free(m.default_project);
	return m.running;
}

/* ----------------------------------------------------------------
 * Status
 * ---------------------------------------------------------------- */

const gchar *
bar_devservices_state_name(BarDevservicesState state)
{
	switch (state) {
	case BAR_DEVSERVICES_OFF:      return "off";
	case BAR_DEVSERVICES_STARTING: return "starting";
	case BAR_DEVSERVICES_UP:       return "up";
	case BAR_DEVSERVICES_STUCK:    return "stuck";
	case BAR_DEVSERVICES_STOPPING: return "stopping";
	case BAR_DEVSERVICES_FAILED:   return "failed";
	default:                       return "?";
	}
}

BarDevservicesState
bar_devservices_status_step(gint *unhealthy, BarDevservicesState previous,
                            gboolean running, gboolean healthy,
                            gint stuck_after)
{
	if (!running) {
		*unhealthy = 0;
		return (previous == BAR_DEVSERVICES_FAILED)
			? BAR_DEVSERVICES_FAILED : BAR_DEVSERVICES_OFF;
	}
	if (previous == BAR_DEVSERVICES_STOPPING)
		return BAR_DEVSERVICES_STOPPING;
	if (healthy) {
		*unhealthy = 0;
		return BAR_DEVSERVICES_UP;
	}
	(*unhealthy)++;
	if (*unhealthy >= MAX(stuck_after, 1))
		return BAR_DEVSERVICES_STUCK;
	return BAR_DEVSERVICES_STARTING;
}

BarDevservicesAgg
bar_devservices_aggregate(const BarDevservicesState *states, guint n,
                          gboolean busy, guint *up)
{
	guint i, n_up = 0;
	gboolean error = FALSE, working = busy;

	for (i = 0; i < n; i++) {
		switch (states[i]) {
		case BAR_DEVSERVICES_UP:
			n_up++;
			break;
		case BAR_DEVSERVICES_STUCK:
		case BAR_DEVSERVICES_FAILED:
			error = TRUE;
			break;
		case BAR_DEVSERVICES_STARTING:
		case BAR_DEVSERVICES_STOPPING:
			working = TRUE;
			break;
		case BAR_DEVSERVICES_OFF:
		default:
			break;
		}
	}
	if (up != NULL)
		*up = n_up;
	if (error)
		return BAR_DEVSERVICES_AGG_ERROR;
	if (working)
		return BAR_DEVSERVICES_AGG_WORKING;
	if (n_up > 0)
		return BAR_DEVSERVICES_AGG_OK;
	return BAR_DEVSERVICES_AGG_OFF;
}

GPtrArray *
bar_devservices_port_clashes(GPtrArray *stacks)
{
	GPtrArray *out;
	GHashTable *seen;
	guint i, j;

	out = g_ptr_array_new_with_free_func(g_free);
	if (stacks == NULL)
		return out;
	seen = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL,
	                             g_free);
	for (i = 0; i < stacks->len; i++) {
		BarDevservicesStack *stack = g_ptr_array_index(stacks, i);

		for (j = 0; j < stack->services->len; j++) {
			BarDevservicesService *svc;
			const gchar *first;
			gpointer key;

			svc = g_ptr_array_index(stack->services, j);
			if (svc->port == 0)
				continue;
			key = GINT_TO_POINTER(svc->port);
			first = g_hash_table_lookup(seen, key);
			if (first != NULL) {
				g_ptr_array_add(out, g_strdup_printf(
					"Port %d: %s and %s/%s", svc->port, first,
					stack->name, svc->id));
				continue;
			}
			g_hash_table_insert(seen, key, g_strdup_printf("%s/%s",
			                    stack->name, svc->id));
		}
	}
	g_hash_table_unref(seen);
	return out;
}

/* ----------------------------------------------------------------
 * Scanning for projects
 * ---------------------------------------------------------------- */

void
bar_devservices_candidate_free(BarDevservicesCandidate *cand)
{
	if (cand == NULL)
		return;
	g_free(cand->dir);
	g_free(cand->key);
	g_ptr_array_unref(cand->found);
	g_free(cand->draft);
	g_free(cand);
}

/* Appends @text as a double-quoted YAML scalar. */
static void
yaml_quote(GString *out, const gchar *text)
{
	const gchar *p;

	g_string_append_c(out, '"');
	for (p = text; *p != '\0'; p++) {
		if (*p == '"' || *p == '\\')
			g_string_append_c(out, '\\');
		if (*p == '\n') {
			g_string_append(out, "\\n");
			continue;
		}
		if ((guchar)*p < 0x20)
			continue;
		g_string_append_c(out, *p);
	}
	g_string_append_c(out, '"');
}

/* A Makefile target, at the start of a line, as `NAME:' but not
   `NAME:=' (a variable). */
static gboolean
makefile_has_target(const gchar *text, const gchar *target)
{
	g_auto(GStrv) lines = NULL;
	gsize len = strlen(target);
	guint i;

	lines = g_strsplit(text, "\n", -1);
	for (i = 0; lines[i] != NULL; i++) {
		const gchar *l = lines[i];

		if (strncmp(l, target, len) != 0)
			continue;
		l += len;
		while (*l == ' ' || *l == '\t')
			l++;
		if (l[0] == ':' && l[1] != '=')
			return TRUE;
	}
	return FALSE;
}

/*
 * compose_ports:
 * @path: a compose file
 * @draft: where the service entries go
 * @found: what was recognised
 *
 * One podman service per compose service, with its first published
 * host port.  `"5432:5432"', `"127.0.0.1:5432:5432"' and `5432' all
 * appear in the wild.
 */
static void
compose_services(const gchar *path, const gchar *file_name, GString *draft,
                 GPtrArray *found)
{
	g_autofree gchar *text = NULL;
	g_autoptr(YamlParser) parser = NULL;
	YamlNode *root;
	YamlMapping *map, *services;
	guint i, n;

	text = read_bounded(path, BAR_DEVSERVICES_MAX_FILE);
	if (text == NULL || !g_utf8_validate(text, -1, NULL))
		return;
	parser = yaml_parser_new();
	if (!yaml_parser_load_from_data(parser, text, -1, NULL))
		return;
	root = yaml_parser_get_root(parser);
	if (root == NULL || yaml_node_get_node_type(root) != YAML_NODE_MAPPING)
		return;
	map = yaml_node_get_mapping(root);
	services = (member(map, "services") != NULL &&
	            yaml_node_get_node_type(member(map, "services"))
	            == YAML_NODE_MAPPING)
		? yaml_node_get_mapping(member(map, "services")) : NULL;
	if (services == NULL)
		return;

	g_ptr_array_add(found, g_strdup_printf("%s", file_name));
	n = MIN(yaml_mapping_get_size(services), 20u);
	for (i = 0; i < n; i++) {
		const gchar *name = yaml_mapping_get_key(services, i);
		YamlNode *svc_node = yaml_mapping_get_value(services, i);
		YamlMapping *svc;
		YamlNode *ports;
		gint port = 0;

		if (!bar_devservices_valid_id(name))
			continue;
		svc = (svc_node != NULL &&
		       yaml_node_get_node_type(svc_node) == YAML_NODE_MAPPING)
			? yaml_node_get_mapping(svc_node) : NULL;
		ports = member(svc, "ports");
		if (ports != NULL &&
		    yaml_node_get_node_type(ports) == YAML_NODE_SEQUENCE &&
		    yaml_sequence_get_length(yaml_node_get_sequence(ports)) > 0) {
			const gchar *spec;

			spec = node_scalar(yaml_sequence_get_element(
				yaml_node_get_sequence(ports), 0));
			if (spec != NULL) {
				g_auto(GStrv) parts = g_strsplit(spec, ":", -1);
				guint np = g_strv_length(parts);
				const gchar *host;

				/* HOST:CONTAINER, IP:HOST:CONTAINER, PORT. */
				host = (np >= 2) ? parts[np - 2] : parts[0];
				port = (gint)g_ascii_strtoll(host, NULL, 10);
				if (port < 1 || port > 65535)
					port = 0;
			}
		}

		g_string_append_printf(draft, "  - id: %s\n", name);
		g_string_append(draft, "    label: ");
		yaml_quote(draft, name);
		g_string_append(draft, "\n    type: podman\n    compose: ");
		yaml_quote(draft, file_name);
		g_string_append_printf(draft, "\n    service: %s\n", name);
		if (port > 0)
			g_string_append_printf(draft,
				"    port: %d\n    health: port\n", port);
	}
}

BarDevservicesCandidate *
bar_devservices_scan_dir(const gchar *dir)
{
	static const gchar *const compose_names[] = {
		"compose.yaml", "compose.yml", "docker-compose.yml",
		"docker-compose.yaml", "podman-compose.yml",
		"podman-compose.yaml", NULL
	};
	static const gchar *const make_names[] = {
		"Makefile", "GNUmakefile", "makefile", NULL
	};
	BarDevservicesCandidate *cand;
	GString *services, *notes, *draft;
	g_autofree gchar *base = NULL;
	gboolean process = FALSE;
	guint i;

	if (dir == NULL || !g_file_test(dir, G_FILE_TEST_IS_DIR))
		return NULL;

	cand = g_new0(BarDevservicesCandidate, 1);
	cand->dir = g_strdup(dir);
	cand->found = g_ptr_array_new_with_free_func(g_free);
	base = g_path_get_basename(dir);
	cand->key = bar_devservices_safe_key(base);
	services = g_string_new(NULL);
	notes = g_string_new(NULL);

	for (i = 0; compose_names[i] != NULL; i++) {
		g_autofree gchar *path = g_build_filename(dir, compose_names[i],
		                                          NULL);

		if (g_file_test(path, G_FILE_TEST_IS_REGULAR)) {
			compose_services(path, compose_names[i], services,
			                 cand->found);
			break;
		}
	}

	/*
	 * One process service at most, by priority: the project's own
	 * Makefile entry point (it usually wraps the rest), then a
	 * package.json dev script, then cargo, then go.  Drafting all of
	 * them would start the same server twice; the others are left as
	 * comments.
	 */
	for (i = 0; make_names[i] != NULL; i++) {
		g_autofree gchar *path = g_build_filename(dir, make_names[i], NULL);
		g_autofree gchar *text = read_bounded(path,
		                                      BAR_DEVSERVICES_MAX_FILE);
		const gchar *target = NULL;

		if (text == NULL)
			continue;
		if (makefile_has_target(text, "dev"))
			target = "dev";
		else if (makefile_has_target(text, "run"))
			target = "run";
		if (target == NULL)
			break;
		g_ptr_array_add(cand->found, g_strdup_printf("%s (%s)",
		                make_names[i], target));
		if (!process) {
			g_string_append_printf(services,
				"  - id: app\n    label: \"make %s\"\n"
				"    type: process\n    cmd: [\"make\", \"%s\"]\n",
				target, target);
			process = TRUE;
		}
		break;
	}

	{
		g_autofree gchar *path = g_build_filename(dir, "package.json",
		                                          NULL);
		g_autofree gchar *text = read_bounded(path,
		                                      BAR_DEVSERVICES_MAX_FILE);
		g_autofree gchar *scripts = NULL;
		g_autofree gchar *dev = NULL;

		if (text != NULL)
			scripts = gowl_bar_json_object(text, "scripts");
		if (scripts != NULL)
			dev = gowl_bar_json_string(scripts, "dev");
		if (dev != NULL) {
			const gchar *runner = "npm";
			g_autofree gchar *pnpm = g_build_filename(dir,
				"pnpm-lock.yaml", NULL);
			g_autofree gchar *yarn = g_build_filename(dir,
				"yarn.lock", NULL);
			g_autofree gchar *bun = g_build_filename(dir,
				"bun.lockb", NULL);

			if (g_file_test(pnpm, G_FILE_TEST_EXISTS))
				runner = "pnpm";
			else if (g_file_test(yarn, G_FILE_TEST_EXISTS))
				runner = "yarn";
			else if (g_file_test(bun, G_FILE_TEST_EXISTS))
				runner = "bun";
			g_ptr_array_add(cand->found,
			                g_strdup("package.json (dev script)"));
			if (!process) {
				g_string_append_printf(services,
					"  - id: web\n    label: \"dev server\"\n"
					"    type: process\n"
					"    cmd: [\"%s\", \"run\", \"dev\"]\n",
					runner);
				process = TRUE;
			} else {
				g_string_append_printf(notes,
					"# package.json has a dev script: "
					"cmd: [\"%s\", \"run\", \"dev\"]\n", runner);
			}
		}
	}

	{
		g_autofree gchar *cargo = g_build_filename(dir, "Cargo.toml", NULL);
		g_autofree gchar *gomod = g_build_filename(dir, "go.mod", NULL);
		g_autofree gchar *py = g_build_filename(dir, "pyproject.toml",
		                                        NULL);

		if (g_file_test(cargo, G_FILE_TEST_IS_REGULAR)) {
			g_ptr_array_add(cand->found, g_strdup("Cargo.toml"));
			if (!process) {
				g_string_append(services,
					"  - id: app\n    label: \"cargo run\"\n"
					"    type: process\n"
					"    cmd: [\"cargo\", \"run\"]\n");
				process = TRUE;
			} else {
				g_string_append(notes, "# Cargo.toml: cmd: "
				                "[\"cargo\", \"run\"]\n");
			}
		}
		if (g_file_test(gomod, G_FILE_TEST_IS_REGULAR)) {
			g_ptr_array_add(cand->found, g_strdup("go.mod"));
			if (!process) {
				g_string_append(services,
					"  - id: app\n    label: \"go run\"\n"
					"    type: process\n"
					"    cmd: [\"go\", \"run\", \".\"]\n");
				process = TRUE;
			} else {
				g_string_append(notes, "# go.mod: cmd: "
				                "[\"go\", \"run\", \".\"]\n");
			}
		}
		if (g_file_test(py, G_FILE_TEST_IS_REGULAR)) {
			/* There is no conventional "run" entry in a
			   pyproject, so this is a note, never a guess
			   that might start the wrong thing. */
			g_ptr_array_add(cand->found, g_strdup("pyproject.toml"));
			g_string_append(notes,
				"# pyproject.toml: add a service for its entry "
				"point, e.g.\n#   cmd: [\"uv\", \"run\", "
				"\"myapp\"]\n");
		}
	}

	if (cand->found->len == 0) {
		g_string_free(services, TRUE);
		g_string_free(notes, TRUE);
		bar_devservices_candidate_free(cand);
		return NULL;
	}

	draft = g_string_new(NULL);
	g_string_append(draft,
		"# Drafted by the gowl devservices widget from the files it "
		"recognised.\n# Nothing here has been run: check every command "
		"before pressing Up.\n# .env files were not read; put the "
		"variables a service needs under env:.\n");
	g_string_append(draft, "name: ");
	yaml_quote(draft, base);
	g_string_append(draft, "\nroot: ");
	yaml_quote(draft, dir);
	g_string_append(draft, "\n");
	if (notes->len > 0)
		g_string_append(draft, notes->str);
	if (services->len > 0) {
		g_string_append(draft, "services:\n");
		g_string_append(draft, services->str);
	} else {
		g_string_append(draft, "services: []\n");
	}
	g_string_append(draft, "tasks: []\n");
	cand->draft = g_string_free(draft, FALSE);
	g_string_free(services, TRUE);
	g_string_free(notes, TRUE);
	return cand;
}

static gint
cmp_candidates(gconstpointer a, gconstpointer b)
{
	const BarDevservicesCandidate *x = *(BarDevservicesCandidate * const *)a;
	const BarDevservicesCandidate *y = *(BarDevservicesCandidate * const *)b;

	return strcmp(x->dir, y->dir);
}

GPtrArray *
bar_devservices_scan(const gchar *search_roots, GPtrArray *stacks, guint limit)
{
	GPtrArray *out;
	g_auto(GStrv) roots = NULL;
	guint r, i;

	out = g_ptr_array_new_with_free_func(
		(GDestroyNotify)bar_devservices_candidate_free);
	if (search_roots == NULL)
		return out;
	roots = g_strsplit(search_roots, ":", 16);
	for (r = 0; roots[r] != NULL && out->len < limit; r++) {
		g_autofree gchar *root = expand_tilde(g_strstrip(roots[r]));
		GDir *d;
		const gchar *name;

		if (root[0] == '\0')
			continue;
		d = g_dir_open(root, 0, NULL);
		if (d == NULL)
			continue;
		while ((name = g_dir_read_name(d)) != NULL && out->len < limit) {
			g_autofree gchar *dir = NULL;
			BarDevservicesCandidate *cand;
			gboolean known = FALSE;

			if (name[0] == '.')
				continue;
			dir = g_build_filename(root, name, NULL);
			if (!g_file_test(dir, G_FILE_TEST_IS_DIR))
				continue;
			for (i = 0; stacks != NULL && i < stacks->len; i++) {
				BarDevservicesStack *s = g_ptr_array_index(stacks, i);

				if (bar_devservices_path_within(s->root, dir) &&
				    bar_devservices_path_within(dir, s->root))
					known = TRUE;
			}
			if (known)
				continue;
			cand = bar_devservices_scan_dir(dir);
			if (cand != NULL)
				g_ptr_array_add(out, cand);
		}
		g_dir_close(d);
	}
	g_ptr_array_sort(out, cmp_candidates);
	return out;
}

gboolean
bar_devservices_write_draft(const gchar *config_dir,
                            const BarDevservicesCandidate *cand,
                            gchar **path_out, GError **error)
{
	g_autofree gchar *path = NULL;
	g_autofree gchar *name = NULL;
	gint fd;
	gsize len, off = 0;

	g_return_val_if_fail(cand != NULL && cand->draft != NULL, FALSE);

	if (!bar_devservices_valid_id(cand->key)) {
		g_set_error(error, BAR_DEVSERVICES_ERROR,
		            BAR_DEVSERVICES_ERROR_INVALID,
		            "`%s' is not a usable stack name", cand->key);
		return FALSE;
	}
	if (g_mkdir_with_parents(config_dir, 0755) != 0) {
		g_set_error(error, BAR_DEVSERVICES_ERROR,
		            BAR_DEVSERVICES_ERROR_IO, "cannot create %s: %s",
		            config_dir, g_strerror(errno));
		return FALSE;
	}
	name = g_strconcat(cand->key, ".yaml", NULL);
	path = g_build_filename(config_dir, name, NULL);
	/* O_EXCL: an existing stack is the user's own edit, and a
	   second Declare must not reset it to the draft. */
	fd = g_open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
	if (fd < 0) {
		g_set_error(error, BAR_DEVSERVICES_ERROR,
		            BAR_DEVSERVICES_ERROR_IO,
		            (errno == EEXIST) ? "%s already exists"
		                              : "cannot write %s", path);
		return FALSE;
	}
	/* The draft holds no secrets, and the user will want to share
	   it; 0644 regardless of a stricter umask. */
	if (fchmod(fd, 0644) != 0)
		g_debug("devservices: fchmod %s: %s", path, g_strerror(errno));
	len = strlen(cand->draft);
	while (off < len) {
		gssize w = write(fd, cand->draft + off, len - off);

		if (w <= 0) {
			close(fd);
			g_unlink(path);
			g_set_error(error, BAR_DEVSERVICES_ERROR,
			            BAR_DEVSERVICES_ERROR_IO, "cannot write %s",
			            path);
			return FALSE;
		}
		off += (gsize)w;
	}
	close(fd);
	if (path_out != NULL)
		*path_out = g_steal_pointer(&path);
	return TRUE;
}
