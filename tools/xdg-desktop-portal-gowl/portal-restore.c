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
 * portal-restore.c - screen-cast grants, in memory and in YAML.
 *
 * The file is small and written whole: a header comment, `version: 1',
 * and a `grants:' sequence of flat mappings whose values are all
 * double-quoted strings.  It is read with libyaml, so a hand edit (the
 * point of keeping it readable) may reformat it however YAML allows;
 * it is written by hand, because the shape is fixed and every scalar
 * is quoted, so there is nothing for an emitter to decide.
 *
 * The write is atomic and 0600 (g_file_set_contents_full): a grant is a
 * standing permission to record the screen, which is not something
 * another user on the machine gets to read or plant.
 */

#include "portal-restore.h"

#include <glib/gstdio.h>
#include <yaml.h>
#include <errno.h>
#include <string.h>

/* The most persistent grants kept.  Every restore mints a new grant and
 * consumes the old one, so the store only grows with distinct
 * applications; past this the least recently used go first. */
#define RESTORE_MAX_GRANTS (64)

struct _PortalRestore {
	gchar     *path;
	GPtrArray *grants;    /* PortalGrant*, both modes */
};

static void
grant_free(gpointer data)
{
	PortalGrant *g = data;

	if (g == NULL)
		return;
	g_free(g->token);
	g_free(g->app_id);
	g_free(g->output_name);
	g_free(g->make);
	g_free(g->model);
	g_free(g->window_app_id);
	g_free(g->window_title);
	g_free(g->stream_id);
	g_free(g);
}

/* A token nobody can guess: 128 random bits, as hex. */
static gchar *
new_token(void)
{
	return g_strdup_printf("%08x%08x%08x%08x", g_random_int(),
	                       g_random_int(), g_random_int(), g_random_int());
}

/* ---------------------------------------------------------------
 * Reading
 * --------------------------------------------------------------- */

static const gchar *
scalar(yaml_document_t *doc, yaml_node_t *node)
{
	(void)doc;
	if (node == NULL || node->type != YAML_SCALAR_NODE)
		return NULL;
	return (const gchar *)node->data.scalar.value;
}

static gint64
parse_time(const gchar *text)
{
	g_autoptr(GDateTime) dt = NULL;

	if (text == NULL)
		return 0;
	dt = g_date_time_new_from_iso8601(text, NULL);
	return dt != NULL ? g_date_time_to_unix(dt) : 0;
}

/* One `- token: ...' mapping into a grant, or NULL if it is not one. */
static PortalGrant *
grant_from_node(yaml_document_t *doc, yaml_node_t *node)
{
	PortalGrant *g;
	yaml_node_pair_t *pair;

	if (node == NULL || node->type != YAML_MAPPING_NODE)
		return NULL;
	g = g_new0(PortalGrant, 1);
	for (pair = node->data.mapping.pairs.start;
	     pair < node->data.mapping.pairs.top; pair++) {
		const gchar *k = scalar(doc, yaml_document_get_node(doc, pair->key));
		const gchar *v = scalar(doc,
			yaml_document_get_node(doc, pair->value));

		if (k == NULL || v == NULL)
			continue;
		if (g_strcmp0(k, "token") == 0)
			g->token = g_strdup(v);
		else if (g_strcmp0(k, "app_id") == 0)
			g->app_id = g_strdup(v);
		else if (g_strcmp0(k, "kind") == 0)
			g->is_window = g_strcmp0(v, "window") == 0;
		else if (g_strcmp0(k, "output_name") == 0)
			g->output_name = g_strdup(v);
		else if (g_strcmp0(k, "make") == 0)
			g->make = g_strdup(v);
		else if (g_strcmp0(k, "model") == 0)
			g->model = g_strdup(v);
		else if (g_strcmp0(k, "window_app_id") == 0)
			g->window_app_id = g_strdup(v);
		else if (g_strcmp0(k, "window_title") == 0)
			g->window_title = g_strdup(v);
		else if (g_strcmp0(k, "stream_id") == 0)
			g->stream_id = g_strdup(v);
		else if (g_strcmp0(k, "created") == 0)
			g->created = parse_time(v);
		else if (g_strcmp0(k, "last_used") == 0)
			g->last_used = parse_time(v);
	}
	/* Only mode-2 grants are ever written, so that is what a read one
	 * is; a token-less or source-less entry is a broken hand edit. */
	g->persist_mode = 2;
	if (g->token == NULL || *g->token == '\0'
	    || (g->is_window ? g->window_app_id == NULL && g->window_title == NULL
	                     : g->output_name == NULL && g->model == NULL)) {
		grant_free(g);
		return NULL;
	}
	if (g->app_id == NULL)
		g->app_id = g_strdup("");
	return g;
}

static void
load(PortalRestore *self)
{
	g_autofree gchar *text = NULL;
	gsize len = 0;
	yaml_parser_t parser;
	yaml_document_t doc;
	yaml_node_t *root;
	yaml_node_pair_t *pair;
	g_autoptr(GError) error = NULL;

	if (!g_file_get_contents(self->path, &text, &len, &error)) {
		if (!g_error_matches(error, G_FILE_ERROR, G_FILE_ERROR_NOENT))
			g_warning("restore: %s", error->message);
		return;
	}
	if (!yaml_parser_initialize(&parser))
		return;
	yaml_parser_set_input_string(&parser, (const unsigned char *)text, len);
	if (!yaml_parser_load(&parser, &doc)) {
		g_warning("restore: %s is not YAML (line %zu: %s); ignoring it",
		          self->path, parser.problem_mark.line + 1,
		          parser.problem != NULL ? parser.problem : "?");
		yaml_parser_delete(&parser);
		return;
	}
	root = yaml_document_get_root_node(&doc);
	if (root != NULL && root->type == YAML_MAPPING_NODE) {
		for (pair = root->data.mapping.pairs.start;
		     pair < root->data.mapping.pairs.top; pair++) {
			yaml_node_t *value;
			yaml_node_item_t *item;

			if (g_strcmp0(scalar(&doc, yaml_document_get_node(&doc,
			                     pair->key)), "grants") != 0)
				continue;
			value = yaml_document_get_node(&doc, pair->value);
			if (value == NULL || value->type != YAML_SEQUENCE_NODE)
				continue;
			for (item = value->data.sequence.items.start;
			     item < value->data.sequence.items.top; item++) {
				PortalGrant *g = grant_from_node(&doc,
					yaml_document_get_node(&doc, *item));

				if (g != NULL)
					g_ptr_array_add(self->grants, g);
			}
		}
	}
	yaml_document_delete(&doc);
	yaml_parser_delete(&parser);
}

/* ---------------------------------------------------------------
 * Writing
 * --------------------------------------------------------------- */

/* A YAML double-quoted scalar: every value is written this way, so a
 * title with a colon, a quote or a leading dash cannot change the
 * document's shape. */
static void
append_quoted(GString *out, const gchar *text)
{
	const guchar *p;

	g_string_append_c(out, '"');
	for (p = (const guchar *)(text != NULL ? text : ""); *p != '\0'; p++) {
		if (*p == '"' || *p == '\\')
			g_string_append_printf(out, "\\%c", *p);
		else if (*p < 0x20 || *p == 0x7f)
			g_string_append_printf(out, "\\x%02x", *p);
		else
			g_string_append_c(out, (gchar)*p);
	}
	g_string_append_c(out, '"');
}

static void
append_field(GString *out, const gchar *key, const gchar *value)
{
	g_string_append_printf(out, "    %s: ", key);
	append_quoted(out, value);
	g_string_append_c(out, '\n');
}

static gchar *
format_time(gint64 t)
{
	g_autoptr(GDateTime) dt = g_date_time_new_from_unix_local(t);

	return dt != NULL ? g_date_time_format_iso8601(dt) : g_strdup("");
}

static void
save(PortalRestore *self)
{
	g_autoptr(GString) out = g_string_new(NULL);
	g_autofree gchar *dir = NULL;
	g_autoptr(GError) error = NULL;
	guint i;

	g_string_append(out,
		"# xdg-desktop-portal-gowl: standing screen-cast permissions.\n"
		"# Each entry lets one application restore a cast without being\n"
		"# asked.  Delete an entry (or this file) to revoke it; the\n"
		"# application is asked again next time.  Written by the portal.\n"
		"version: 1\n"
		"grants:\n");
	for (i = 0; i < self->grants->len; i++) {
		const PortalGrant *g = g_ptr_array_index(self->grants, i);
		g_autofree gchar *created = NULL;
		g_autofree gchar *used = NULL;

		if (g->persist_mode != 2)
			continue;
		created = format_time(g->created);
		used = format_time(g->last_used);
		g_string_append(out, "  - token: ");
		append_quoted(out, g->token);
		g_string_append_c(out, '\n');
		append_field(out, "app_id", g->app_id);
		append_field(out, "kind", g->is_window ? "window" : "output");
		if (g->is_window) {
			append_field(out, "window_app_id", g->window_app_id);
			append_field(out, "window_title", g->window_title);
		} else {
			append_field(out, "output_name", g->output_name);
			append_field(out, "make", g->make);
			append_field(out, "model", g->model);
		}
		append_field(out, "stream_id", g->stream_id);
		append_field(out, "created", created);
		append_field(out, "last_used", used);
	}

	dir = g_path_get_dirname(self->path);
	if (g_mkdir_with_parents(dir, 0700) != 0) {
		g_warning("restore: cannot create %s: %s", dir, g_strerror(errno));
		return;
	}
	if (!g_file_set_contents_full(self->path, out->str, (gssize)out->len,
	                              G_FILE_SET_CONTENTS_CONSISTENT, 0600,
	                              &error))
		g_warning("restore: cannot save %s: %s", self->path,
		          error->message);
}

/* ---------------------------------------------------------------
 * Matching
 * --------------------------------------------------------------- */

static gboolean
same(const gchar *a, const gchar *b)
{
	return g_strcmp0(a != NULL ? a : "", b != NULL ? b : "") == 0;
}

static gboolean
blank(const gchar *s)
{
	return s == NULL || *s == '\0';
}

const PortalCaptureSource *
portal_restore_match(const PortalGrant *grant, GPtrArray *sources)
{
	const PortalCaptureSource *loose = NULL;
	guint n_loose = 0;
	guint i;

	if (grant == NULL || sources == NULL)
		return NULL;

	for (i = 0; i < sources->len; i++) {
		const PortalCaptureSource *s = g_ptr_array_index(sources, i);

		if (s->is_window != grant->is_window)
			continue;
		if (grant->is_window) {
			/* Exact: the same application and the same title. */
			if (same(s->app_id, grant->window_app_id)
			    && same(s->window_title, grant->window_title))
				return s;
			/* Loose: the same application, if it is the only
			 * window it has.  Never for a window with no app id,
			 * where "the same application" means nothing. */
			if (!blank(grant->window_app_id)
			    && same(s->app_id, grant->window_app_id)) {
				loose = s;
				n_loose++;
			}
		} else {
			gboolean monitor_same = same(s->make, grant->make)
			                        && same(s->model, grant->model);

			/* Exact: the connector, and the monitor on it unless
			 * the grant never knew which monitor that was. */
			if (same(s->output_name, grant->output_name)
			    && (monitor_same
			        || (blank(grant->make) && blank(grant->model))))
				return s;
			/* Loose: the same monitor on another connector. */
			if (monitor_same && !blank(grant->model)) {
				loose = s;
				n_loose++;
			}
		}
	}
	return n_loose == 1 ? loose : NULL;
}

/* ---------------------------------------------------------------
 * The store
 * --------------------------------------------------------------- */

PortalRestore *
portal_restore_new(const gchar *path)
{
	PortalRestore *self = g_new0(PortalRestore, 1);

	self->path = path != NULL
		? g_strdup(path)
		: g_build_filename(g_get_user_state_dir(), "gowl",
		                   "screencast-grants.yaml", NULL);
	self->grants = g_ptr_array_new_with_free_func(grant_free);
	load(self);
	g_debug("restore: %u persistent grants in %s", self->grants->len,
	        self->path);
	return self;
}

void
portal_restore_free(PortalRestore *self)
{
	if (self == NULL)
		return;
	g_clear_pointer(&self->grants, g_ptr_array_unref);
	g_free(self->path);
	g_free(self);
}

const gchar *
portal_restore_get_path(PortalRestore *self)
{
	return self != NULL ? self->path : NULL;
}

const PortalGrant *
portal_restore_lookup(PortalRestore *self, const gchar *token,
                      const gchar *app_id)
{
	guint i;

	if (self == NULL || blank(token))
		return NULL;
	for (i = 0; i < self->grants->len; i++) {
		const PortalGrant *g = g_ptr_array_index(self->grants, i);

		if (g_strcmp0(g->token, token) != 0)
			continue;
		/* Bound to who it was given to: a token is not a bearer
		 * credential for some other application. */
		if (!same(g->app_id, app_id)) {
			g_message("restore: a grant for '%s' was presented by "
			          "'%s'; ignoring it", g->app_id,
			          app_id != NULL ? app_id : "");
			return NULL;
		}
		return g;
	}
	return NULL;
}

/* Drops the least recently used persistent grants past the cap. */
static void
prune(PortalRestore *self)
{
	for (;;) {
		guint i;
		guint n = 0;
		gint oldest = -1;
		gint64 oldest_t = G_MAXINT64;

		for (i = 0; i < self->grants->len; i++) {
			const PortalGrant *g = g_ptr_array_index(self->grants, i);

			if (g->persist_mode != 2)
				continue;
			n++;
			if (g->last_used < oldest_t) {
				oldest_t = g->last_used;
				oldest = (gint)i;
			}
		}
		if (n <= RESTORE_MAX_GRANTS || oldest < 0)
			return;
		g_ptr_array_remove_index(self->grants, (guint)oldest);
	}
}

const PortalGrant *
portal_restore_add(PortalRestore *self, const gchar *app_id,
                   guint32 persist_mode, const PortalCaptureSource *source,
                   const gchar *stream_id)
{
	PortalGrant *g;

	g_return_val_if_fail(self != NULL, NULL);
	g_return_val_if_fail(source != NULL, NULL);
	g_return_val_if_fail(persist_mode == 1 || persist_mode == 2, NULL);

	g = g_new0(PortalGrant, 1);
	g->token = new_token();
	g->app_id = g_strdup(app_id != NULL ? app_id : "");
	g->persist_mode = persist_mode;
	g->is_window = source->is_window;
	if (source->is_window) {
		g->window_app_id = g_strdup(source->app_id);
		g->window_title = g_strdup(source->window_title);
	} else {
		g->output_name = g_strdup(source->output_name);
		g->make = g_strdup(source->make);
		g->model = g_strdup(source->model);
	}
	g->stream_id = !blank(stream_id) ? g_strdup(stream_id) : new_token();
	g->created = g_get_real_time() / G_USEC_PER_SEC;
	g->last_used = g->created;
	g_ptr_array_add(self->grants, g);
	if (persist_mode == 2) {
		prune(self);
		save(self);
	}
	return g;
}

gboolean
portal_restore_remove(PortalRestore *self, const gchar *token)
{
	guint i;

	if (self == NULL || blank(token))
		return FALSE;
	for (i = 0; i < self->grants->len; i++) {
		const PortalGrant *g = g_ptr_array_index(self->grants, i);

		if (g_strcmp0(g->token, token) == 0) {
			gboolean persistent = g->persist_mode == 2;

			g_ptr_array_remove_index(self->grants, i);
			if (persistent)
				save(self);
			return TRUE;
		}
	}
	return FALSE;
}
