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
 * The pure half of the `ai-assistant' widget.  See
 * bar-assistant-model.h for what each function promises; the comments
 * here are about how.
 *
 * Every string that arrives from outside --- ollama's JSON, a /proc
 * file, a model's answer --- is made valid UTF-8 and length-capped
 * before it leaves this file, because the next stop for most of them
 * is pango, which does not forgive invalid text.
 */

#include "bar-assistant-model.h"
#include "bar-util.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sys/statvfs.h>
#include <unistd.h>

/* The longest string field read out of an ollama reply.  A model name
   is under a hundred bytes; anything longer is not a model name. */
#define JSON_MAX_STRING (4096)

/* How many /proc entries the process scan looks at before giving up.
   A machine with more processes than this is not one where the
   assistant's prompt needs a complete list. */
#define PROC_SCAN_MAX (8192)

/* ── The conversation ─────────────────────────────────────────────── */

struct _BarAssistantTranscript {
	GPtrArray *turns;   /* BarAssistantTurn*, oldest first */
};

static void
turn_free(gpointer data)
{
	BarAssistantTurn *turn = data;

	if (turn == NULL)
		return;
	g_free(turn->text);
	g_free(turn);
}

/**
 * bar_assistant_transcript_new:
 *
 * Returns: (transfer full): an empty transcript
 */
BarAssistantTranscript *
bar_assistant_transcript_new(void)
{
	BarAssistantTranscript *self;

	self = g_new0(BarAssistantTranscript, 1);
	self->turns = g_ptr_array_new_with_free_func(turn_free);
	return self;
}

/**
 * bar_assistant_transcript_free:
 * @self: (nullable): a transcript
 */
void
bar_assistant_transcript_free(BarAssistantTranscript *self)
{
	if (self == NULL)
		return;
	g_ptr_array_unref(self->turns);
	g_free(self);
}

/**
 * bar_assistant_transcript_clear:
 * @self: a transcript
 */
void
bar_assistant_transcript_clear(BarAssistantTranscript *self)
{
	g_return_if_fail(self != NULL);
	g_ptr_array_set_size(self->turns, 0);
}

/*
 * Valid UTF-8 no longer than @max bytes, cut on a character boundary.
 * A cut in the middle of a multi-byte character would hand pango
 * exactly the invalid text this is here to prevent.
 */
static gchar *
clean_text(const gchar *text, gsize max)
{
	gchar *valid;
	gsize len;

	valid = g_utf8_make_valid((text != NULL) ? text : "", -1);
	len = strlen(valid);
	if (max > 0 && len > max) {
		const gchar *end;

		end = g_utf8_find_prev_char(valid, valid + max + 1);
		if (end == NULL)
			end = valid;
		valid[end - valid] = '\0';
	}
	return valid;
}

void
bar_assistant_transcript_add(BarAssistantTranscript *self,
                             BarAssistantRole role, const gchar *text)
{
	BarAssistantTurn *turn;

	g_return_if_fail(self != NULL);

	turn = g_new0(BarAssistantTurn, 1);
	turn->role = role;
	turn->text = clean_text(text, BAR_ASSISTANT_MAX_MESSAGE);
	g_ptr_array_add(self->turns, turn);
}

void
bar_assistant_transcript_pop(BarAssistantTranscript *self)
{
	g_return_if_fail(self != NULL);
	if (self->turns->len > 0)
		g_ptr_array_remove_index(self->turns, self->turns->len - 1);
}

void
bar_assistant_transcript_trim(BarAssistantTranscript *self,
                              guint max_messages)
{
	const BarAssistantTurn *first;

	g_return_if_fail(self != NULL);

	if (self->turns->len > max_messages)
		g_ptr_array_remove_range(self->turns, 0,
		                         self->turns->len - max_messages);

	/* An answer with its question cut off is noise, and worse, it is
	   noise the model is shown as something it said. */
	if (self->turns->len > 0) {
		first = g_ptr_array_index(self->turns, 0);
		if (first->role == BAR_ASSISTANT_ROLE_ASSISTANT)
			g_ptr_array_remove_index(self->turns, 0);
	}
}

/**
 * bar_assistant_transcript_len:
 * @self: a transcript
 *
 * Returns: how many messages it holds
 */
guint
bar_assistant_transcript_len(BarAssistantTranscript *self)
{
	g_return_val_if_fail(self != NULL, 0);
	return self->turns->len;
}

/**
 * bar_assistant_transcript_get:
 * @self: a transcript
 * @index: a message index, oldest first
 *
 * Returns: (transfer none) (nullable): the message
 */
const BarAssistantTurn *
bar_assistant_transcript_get(BarAssistantTranscript *self, guint index)
{
	g_return_val_if_fail(self != NULL, NULL);
	if (index >= self->turns->len)
		return NULL;
	return g_ptr_array_index(self->turns, index);
}

static gchar *
turn_piece(BarAssistantRole role, const gchar *text)
{
	return g_strdup_printf("%s: %s",
		(role == BAR_ASSISTANT_ROLE_USER) ? "User" : "Assistant",
		(text != NULL) ? text : "");
}

gchar *
bar_assistant_transcript_flatten(BarAssistantTranscript *self,
                                 const gchar *next_user, gsize max_bytes)
{
	g_autoptr(GPtrArray) pieces = NULL;
	g_autofree gchar *next = NULL;
	GString *out;
	gsize total;
	guint i, start;

	g_return_val_if_fail(self != NULL, NULL);

	/* Each message as the line the model will read. */
	pieces = g_ptr_array_new_with_free_func(g_free);
	total = 0;
	for (i = 0; i < self->turns->len; i++) {
		const BarAssistantTurn *turn = g_ptr_array_index(self->turns, i);
		gchar *piece = turn_piece(turn->role, turn->text);

		total += strlen(piece) + 2;
		g_ptr_array_add(pieces, piece);
	}
	if (next_user != NULL) {
		g_autofree gchar *clean = clean_text(next_user,
		                                     BAR_ASSISTANT_MAX_MESSAGE);

		next = turn_piece(BAR_ASSISTANT_ROLE_USER, clean);
		total += strlen(next);
	}

	/* Over budget: forget from the oldest end.  The question being
	   asked is never dropped --- without it there is nothing to
	   answer. */
	start = 0;
	while (max_bytes > 0 && total > max_bytes && start < pieces->len) {
		total -= strlen(g_ptr_array_index(pieces, start)) + 2;
		start++;
	}

	out = g_string_new(NULL);
	for (i = start; i < pieces->len; i++) {
		if (out->len > 0)
			g_string_append(out, "\n\n");
		g_string_append(out, g_ptr_array_index(pieces, i));
	}
	if (next != NULL) {
		if (out->len > 0)
			g_string_append(out, "\n\n");
		g_string_append(out, next);
	}
	return g_string_free(out, FALSE);
}

/* ── Names ────────────────────────────────────────────────────────── */

gboolean
bar_assistant_valid_provider(const gchar *name)
{
	const gchar *p;
	gsize len;

	if (name == NULL || name[0] == '\0' || name[0] == '-')
		return FALSE;
	len = strlen(name);
	if (len > 64)
		return FALSE;
	for (p = name; *p != '\0'; p++) {
		if (!g_ascii_isalnum(*p) && *p != '.' && *p != '_' &&
		    *p != '-')
			return FALSE;
	}
	return TRUE;
}

gboolean
bar_assistant_valid_model(const gchar *name)
{
	const gchar *p;
	gsize len;

	if (name == NULL || name[0] == '\0')
		return FALSE;
	/* Leading `-' is an option to curl or ai; a leading `/' or `.'
	   is a path, never a model. */
	if (name[0] == '-' || name[0] == '/' || name[0] == '.')
		return FALSE;
	len = strlen(name);
	if (len > 200 || strstr(name, "..") != NULL)
		return FALSE;
	for (p = name; *p != '\0'; p++) {
		if (!g_ascii_isalnum(*p) && strchr("._:/@+-", *p) == NULL)
			return FALSE;
	}
	return TRUE;
}

/* Append @name to @out unless it is already there. */
static void
add_unique(GPtrArray *out, const gchar *name)
{
	guint i;

	for (i = 0; i < out->len; i++)
		if (g_strcmp0(g_ptr_array_index(out, i), name) == 0)
			return;
	g_ptr_array_add(out, g_strdup(name));
}

gchar **
bar_assistant_parse_provider_listing(const gchar *text)
{
	g_auto(GStrv) lines = NULL;
	GPtrArray *out;
	gboolean in_list;
	guint i;

	out = g_ptr_array_new();
	if (text == NULL) {
		g_ptr_array_add(out, NULL);
		return (gchar **)g_ptr_array_free(out, FALSE);
	}

	/*
	 * `ai --list-providers' prints a `Providers:' header and then one
	 * indented line per provider: the name, its kind, sometimes a
	 * note in parentheses.  Only the first word is the name.  A line
	 * that is not indented after the header ends the list, so a
	 * trailing hint or a second section is not read as providers.
	 */
	lines = g_strsplit(text, "\n", 256);
	in_list = (strstr(text, "Providers:") == NULL);
	for (i = 0; lines[i] != NULL && out->len < 128; i++) {
		const gchar *line = lines[i];
		g_autofree gchar *word = NULL;
		const gchar *end;

		if (g_str_has_prefix(line, "Providers:")) {
			in_list = TRUE;
			continue;
		}
		if (!in_list || line[0] == '\0')
			continue;
		if (line[0] != ' ' && line[0] != '\t') {
			in_list = FALSE;
			continue;
		}
		while (*line == ' ' || *line == '\t')
			line++;
		end = line;
		while (*end != '\0' && !g_ascii_isspace(*end))
			end++;
		word = g_strndup(line, (gsize)(end - line));
		if (bar_assistant_valid_provider(word))
			add_unique(out, word);
	}
	g_ptr_array_add(out, NULL);
	return (gchar **)g_ptr_array_free(out, FALSE);
}

gchar **
bar_assistant_split_list(const gchar *csv, gboolean models)
{
	g_auto(GStrv) parts = NULL;
	GPtrArray *out;
	guint i;

	out = g_ptr_array_new();
	if (csv != NULL && csv[0] != '\0') {
		parts = g_strsplit(csv, ",", 512);
		for (i = 0; parts[i] != NULL && out->len < 128; i++) {
			gchar *item = g_strstrip(parts[i]);
			gboolean ok;

			ok = models ? bar_assistant_valid_model(item)
			            : bar_assistant_valid_provider(item);
			if (ok)
				add_unique(out, item);
		}
	}
	g_ptr_array_add(out, NULL);
	return (gchar **)g_ptr_array_free(out, FALSE);
}

/* ── A small, bounded JSON reader ─────────────────────────────────── */

/*
 * Why not gowl-bar-json.h: it finds a key ANYWHERE after the start of
 * the fragment it is given, and an ollama model object nests a
 * `details' object of its own.  Every read here is at depth one of a
 * named object, so a `size' or `name' inside `details' --- or inside
 * a hostile string --- can never be taken for the model's.
 */

/* @p is at an opening quote; returns just past the closing one. */
static const gchar *
json_skip_string(const gchar *p)
{
	p++;
	while (*p != '\0') {
		if (*p == '\\') {
			if (p[1] == '\0')
				return NULL;
			p += 2;
			continue;
		}
		if (*p == '"')
			return p + 1;
		p++;
	}
	return NULL;
}

/* @p is at the start of any value; returns just past it. */
static const gchar *
json_skip_value(const gchar *p)
{
	gint depth;

	while (*p != '\0' && g_ascii_isspace(*p))
		p++;
	if (*p == '"')
		return json_skip_string(p);
	if (*p != '{' && *p != '[') {
		/* A number or a literal: up to the next delimiter. */
		while (*p != '\0' && *p != ',' && *p != '}' && *p != ']' &&
		       !g_ascii_isspace(*p))
			p++;
		return p;
	}

	depth = 0;
	while (*p != '\0') {
		if (*p == '"') {
			p = json_skip_string(p);
			if (p == NULL)
				return NULL;
			continue;
		}
		if (*p == '{' || *p == '[')
			depth++;
		else if (*p == '}' || *p == ']') {
			depth--;
			if (depth == 0)
				return p + 1;
		}
		p++;
	}
	return NULL;
}

/* In the object starting at @obj (its `{'), the value of @key at depth
   one, or NULL. */
static const gchar *
json_top_value(const gchar *obj, const gchar *key)
{
	const gchar *p;
	gsize klen;
	gint depth;

	if (obj == NULL)
		return NULL;
	while (*obj != '\0' && g_ascii_isspace(*obj))
		obj++;
	if (*obj != '{')
		return NULL;

	klen  = strlen(key);
	depth = 0;
	p     = obj;
	while (*p != '\0') {
		if (*p == '"') {
			const gchar *start = p + 1;
			const gchar *end = json_skip_string(p);

			if (end == NULL)
				return NULL;
			if (depth == 1 && (gsize)(end - 1 - start) == klen &&
			    strncmp(start, key, klen) == 0) {
				const gchar *q = end;

				while (*q != '\0' && g_ascii_isspace(*q))
					q++;
				/* A string followed by `:' is a key; a string
				   VALUE equal to @key is followed by `,'. */
				if (*q == ':') {
					q++;
					while (*q != '\0' && g_ascii_isspace(*q))
						q++;
					return q;
				}
			}
			p = end;
			continue;
		}
		if (*p == '{' || *p == '[')
			depth++;
		else if (*p == '}' || *p == ']') {
			depth--;
			if (depth <= 0)
				return NULL;
		}
		p++;
	}
	return NULL;
}

/* The string at @p, unescaped, valid UTF-8 and at most
   JSON_MAX_STRING bytes; NULL when @p is not a string. */
static gchar *
json_read_string(const gchar *p)
{
	GString *out;
	g_autofree gchar *raw = NULL;

	if (p == NULL || *p != '"')
		return NULL;
	p++;

	out = g_string_new(NULL);
	while (*p != '\0' && *p != '"') {
		if (out->len > JSON_MAX_STRING)
			break;
		if (*p != '\\') {
			g_string_append_c(out, *p);
			p++;
			continue;
		}
		p++;
		switch (*p) {
		case 'n':  g_string_append_c(out, '\n'); break;
		case 't':  g_string_append_c(out, '\t'); break;
		case 'r':  g_string_append_c(out, '\r'); break;
		case 'b':  g_string_append_c(out, '\b'); break;
		case 'f':  g_string_append_c(out, '\f'); break;
		case 'u': {
			gchar hex[5];
			gunichar c;

			if (strlen(p) < 5) {
				p = p + strlen(p);
				continue;
			}
			memcpy(hex, p + 1, 4);
			hex[4] = '\0';
			c = (gunichar)strtoul(hex, NULL, 16);
			/* A lone surrogate is not a character; show the
			   replacement rather than encoding garbage. */
			if (c >= 0xD800 && c <= 0xDFFF)
				c = 0xFFFD;
			if (c == 0)
				c = 0xFFFD;
			g_string_append_unichar(out, c);
			p += 4;
			break;
		}
		case '\0':
			continue;
		default:
			/* \" \\ \/ and anything unknown: the character. */
			g_string_append_c(out, *p);
			break;
		}
		p++;
	}

	raw = g_string_free(out, FALSE);
	return clean_text(raw, JSON_MAX_STRING);
}

/* An unsigned integer at @p (a float is truncated); 0 when not a
   number or negative. */
static guint64
json_read_uint64(const gchar *p)
{
	gchar *end = NULL;
	gchar *int_end = NULL;
	gdouble d;
	guint64 whole;

	if (p == NULL || !(g_ascii_isdigit(*p)))
		return 0;
	d = g_ascii_strtod(p, &end);
	if (end == p || d < 0.0 || isnan(d))
		return 0;
	/* Plain digits are read exactly; only `1.5e9' and the like go
	   through the double, where a byte either way does not matter. */
	whole = g_ascii_strtoull(p, &int_end, 10);
	if (int_end == end)
		return whole;
	if (d >= 18446744073709551615.0)
		return G_MAXUINT64;
	return (guint64)d;
}

static gchar *
json_top_string(const gchar *obj, const gchar *key)
{
	return json_read_string(json_top_value(obj, key));
}

static guint64
json_top_uint64(const gchar *obj, const gchar *key)
{
	return json_read_uint64(json_top_value(obj, key));
}

/* ── ollama ───────────────────────────────────────────────────────── */

/**
 * bar_assistant_model_free:
 * @model: (nullable): a model
 */
void
bar_assistant_model_free(BarAssistantModel *model)
{
	if (model == NULL)
		return;
	g_free(model->name);
	g_free(model->family);
	g_free(model->params);
	g_free(model->quant);
	g_free(model->expires_at);
	g_free(model);
}

/* One model object, read at its own top level. */
static BarAssistantModel *
parse_one_model(const gchar *obj)
{
	BarAssistantModel *model;
	const gchar *details;
	gchar *name;

	name = json_top_string(obj, "name");
	if (name == NULL || name[0] == '\0') {
		/* Newer servers also send `model'; older ones only `name'. */
		g_free(name);
		name = json_top_string(obj, "model");
	}
	if (name == NULL || name[0] == '\0') {
		g_free(name);
		return NULL;
	}

	model = g_new0(BarAssistantModel, 1);
	model->name       = name;
	model->size       = json_top_uint64(obj, "size");
	model->size_vram  = json_top_uint64(obj, "size_vram");
	model->expires_at = json_top_string(obj, "expires_at");

	details = json_top_value(obj, "details");
	if (details != NULL && *details == '{') {
		model->family = json_top_string(details, "family");
		model->params = json_top_string(details, "parameter_size");
		model->quant  = json_top_string(details, "quantization_level");
	}
	return model;
}

GPtrArray *
bar_assistant_parse_models(const gchar *json, gchar **error)
{
	GPtrArray *models;
	const gchar *p;

	if (error != NULL)
		*error = NULL;

	p = json_top_value(json, "models");
	if (p == NULL || *p != '[') {
		/* An HTML error page, a proxy's message, an older or newer
		   API --- not something to guess a model list out of. */
		g_autofree gchar *err = json_top_string(json, "error");

		if (error != NULL)
			*error = g_strdup((err != NULL && err[0] != '\0')
				? err : "not an ollama reply (no model list)");
		return NULL;
	}

	models = g_ptr_array_new_with_free_func(
		(GDestroyNotify)bar_assistant_model_free);
	p++;
	while (*p != '\0' && models->len < BAR_ASSISTANT_MAX_MODELS) {
		const gchar *end;

		while (*p != '\0' && (g_ascii_isspace(*p) || *p == ','))
			p++;
		if (*p == ']' || *p == '\0')
			break;
		end = json_skip_value(p);
		if (end == NULL) {
			/* Truncated mid-object: keep what was complete. */
			break;
		}
		if (*p == '{') {
			g_autofree gchar *obj = g_strndup(p, (gsize)(end - p));
			BarAssistantModel *model = parse_one_model(obj);

			if (model != NULL)
				g_ptr_array_add(models, model);
		}
		p = end;
	}
	return models;
}

gboolean
bar_assistant_parse_pull_line(const gchar *line, BarAssistantPull *out)
{
	g_return_val_if_fail(out != NULL, FALSE);

	memset(out, 0, sizeof(*out));
	if (line == NULL)
		return FALSE;
	while (*line != '\0' && g_ascii_isspace(*line))
		line++;
	if (*line != '{')
		return FALSE;

	out->status    = json_top_string(line, "status");
	out->error     = json_top_string(line, "error");
	out->total     = json_top_uint64(line, "total");
	out->completed = json_top_uint64(line, "completed");
	out->done      = (g_strcmp0(out->status, "success") == 0);
	if (out->completed > out->total)
		out->completed = out->total;
	return (out->status != NULL || out->error != NULL);
}

/**
 * bar_assistant_pull_clear:
 * @pull: a reading
 */
void
bar_assistant_pull_clear(BarAssistantPull *pull)
{
	if (pull == NULL)
		return;
	g_clear_pointer(&pull->status, g_free);
	g_clear_pointer(&pull->error, g_free);
}

gchar *
bar_assistant_json_escape(const gchar *text)
{
	GString *out;
	const gchar *p;

	out = g_string_new(NULL);
	if (text == NULL)
		return g_string_free(out, FALSE);
	for (p = text; *p != '\0'; p++) {
		guchar c = (guchar)*p;

		switch (c) {
		case '"':  g_string_append(out, "\\\""); break;
		case '\\': g_string_append(out, "\\\\"); break;
		case '\n': g_string_append(out, "\\n"); break;
		case '\r': g_string_append(out, "\\r"); break;
		case '\t': g_string_append(out, "\\t"); break;
		default:
			if (c < 0x20 || c == 0x7f)
				g_string_append_printf(out, "\\u%04x", c);
			else
				g_string_append_c(out, (gchar)c);
			break;
		}
	}
	return g_string_free(out, FALSE);
}

gchar *
bar_assistant_normalise_url(const gchar *url)
{
	gchar *out;
	const gchar *p;
	gsize len;

	if (url == NULL)
		return NULL;
	out = g_strstrip(g_strdup(url));
	if (!g_str_has_prefix(out, "http://") &&
	    !g_str_has_prefix(out, "https://")) {
		g_free(out);
		return NULL;
	}
	for (p = out; *p != '\0'; p++) {
		if ((guchar)*p <= 0x20 || (guchar)*p == 0x7f) {
			g_free(out);
			return NULL;
		}
	}
	len = strlen(out);
	while (len > 0 && out[len - 1] == '/')
		out[--len] = '\0';
	if (len <= strlen("https://") || len > 512) {
		g_free(out);
		return NULL;
	}
	return out;
}

/*
 * The keep_alive value as JSON.  ollama takes a duration string
 * (`30m', `2h'), a number of seconds, or -1 for "never unload".
 * Anything else falls back to half an hour rather than reaching the
 * body unchecked.
 */
static gchar *
keep_alive_json(const gchar *spec)
{
	const gchar *p;
	gsize len;

	if (spec == NULL || spec[0] == '\0')
		return g_strdup("\"30m\"");
	if (strcmp(spec, "-1") == 0)
		return g_strdup("-1");
	len = strlen(spec);
	if (len > 8)
		return g_strdup("\"30m\"");
	for (p = spec; *p != '\0'; p++) {
		gboolean last = (p[1] == '\0');

		if (g_ascii_isdigit(*p))
			continue;
		if (last && p != spec && strchr("smh", *p) != NULL)
			return g_strdup_printf("\"%s\"", spec);
		return g_strdup("\"30m\"");
	}
	return g_strdup(spec);
}

gchar **
bar_assistant_curl_argv(const gchar *curl, const gchar *base_url,
                        BarAssistantOp op, const gchar *model,
                        const gchar *keep_alive)
{
	GPtrArray *argv;
	g_autofree gchar *name = NULL;
	g_autofree gchar *ka = NULL;
	const gchar *path = NULL;
	const gchar *method = NULL;
	const gchar *max_time = NULL;
	gchar *body = NULL;

	g_return_val_if_fail(base_url != NULL, NULL);

	switch (op) {
	case BAR_ASSISTANT_OP_TAGS:
		path = "/api/tags";
		max_time = "5";
		break;
	case BAR_ASSISTANT_OP_PS:
		path = "/api/ps";
		max_time = "5";
		break;
	case BAR_ASSISTANT_OP_LOAD:
	case BAR_ASSISTANT_OP_UNLOAD:
	case BAR_ASSISTANT_OP_DELETE:
	case BAR_ASSISTANT_OP_PULL:
		if (!bar_assistant_valid_model(model))
			return NULL;
		name = bar_assistant_json_escape(model);
		break;
	case BAR_ASSISTANT_OP_NONE:
	case BAR_ASSISTANT_OP_UNLOAD_ALL:
	default:
		return NULL;
	}

	/*
	 * The bodies.  A load is a generate with no prompt, which ollama
	 * answers by loading the model and returning; an unload is the
	 * same with keep_alive 0.  Delete and pull send the model under
	 * both `model' (current servers) and `name' (older ones).  Every
	 * body starts with `{', so curl's `--data-binary @file' form can
	 * never be reached through it.
	 */
	switch (op) {
	case BAR_ASSISTANT_OP_LOAD:
		ka = keep_alive_json(keep_alive);
		body = g_strdup_printf("{\"model\":\"%s\",\"prompt\":\"\","
		                       "\"keep_alive\":%s,\"stream\":false}",
		                       name, ka);
		path = "/api/generate";
		method = "POST";
		/* Loading a large model from a slow disk takes minutes. */
		max_time = "300";
		break;
	case BAR_ASSISTANT_OP_UNLOAD:
		body = g_strdup_printf("{\"model\":\"%s\",\"prompt\":\"\","
		                       "\"keep_alive\":0,\"stream\":false}",
		                       name);
		path = "/api/generate";
		method = "POST";
		max_time = "60";
		break;
	case BAR_ASSISTANT_OP_DELETE:
		body = g_strdup_printf("{\"model\":\"%s\",\"name\":\"%s\"}",
		                       name, name);
		path = "/api/delete";
		method = "DELETE";
		max_time = "30";
		break;
	case BAR_ASSISTANT_OP_PULL:
		body = g_strdup_printf("{\"model\":\"%s\",\"name\":\"%s\","
		                       "\"stream\":true}", name, name);
		path = "/api/pull";
		method = "POST";
		/* No overall limit: a download takes as long as it takes,
		   and the user has a Cancel button. */
		max_time = NULL;
		break;
	case BAR_ASSISTANT_OP_NONE:
	case BAR_ASSISTANT_OP_TAGS:
	case BAR_ASSISTANT_OP_PS:
	case BAR_ASSISTANT_OP_UNLOAD_ALL:
	default:
		break;
	}

	argv = g_ptr_array_new();
	g_ptr_array_add(argv, g_strdup((curl != NULL && curl[0] != '\0')
	                               ? curl : "curl"));
	g_ptr_array_add(argv, g_strdup("-fsS"));
	if (op == BAR_ASSISTANT_OP_PULL) {
		/* -N: hand each progress line over as it arrives. */
		g_ptr_array_add(argv, g_strdup("-N"));
		g_ptr_array_add(argv, g_strdup("--connect-timeout"));
		g_ptr_array_add(argv, g_strdup("5"));
	}
	if (max_time != NULL) {
		g_ptr_array_add(argv, g_strdup("--max-time"));
		g_ptr_array_add(argv, g_strdup(max_time));
	}
	if (op == BAR_ASSISTANT_OP_LOAD || op == BAR_ASSISTANT_OP_UNLOAD ||
	    op == BAR_ASSISTANT_OP_DELETE) {
		/*
		 * A successful delete has an EMPTY body, and the bar's
		 * runner reads empty output as failure.  The status code on
		 * stdout makes success say something; -f already turns
		 * every 4xx/5xx into a non-zero exit.
		 */
		g_ptr_array_add(argv, g_strdup("--write-out"));
		g_ptr_array_add(argv, g_strdup("%{http_code}\\n"));
	}
	if (method != NULL) {
		g_ptr_array_add(argv, g_strdup("-X"));
		g_ptr_array_add(argv, g_strdup(method));
		g_ptr_array_add(argv, g_strdup("-H"));
		g_ptr_array_add(argv, g_strdup("Content-Type: application/json"));
		g_ptr_array_add(argv, g_strdup("--data-binary"));
		g_ptr_array_add(argv, body);
		body = NULL;
	}
	g_ptr_array_add(argv, g_strdup("--url"));
	g_ptr_array_add(argv, g_strconcat(base_url, path, NULL));
	g_ptr_array_add(argv, NULL);
	g_free(body);
	return (gchar **)g_ptr_array_free(argv, FALSE);
}

/* ── Memory, and whether a model fits ─────────────────────────────── */

/**
 * bar_assistant_machine_clear:
 * @machine: a reading
 */
void
bar_assistant_machine_clear(BarAssistantMachine *machine)
{
	if (machine == NULL)
		return;
	g_clear_pointer(&machine->gpu_kind, g_free);
	g_clear_pointer(&machine->top, g_strfreev);
	memset(machine, 0, sizeof(*machine));
}

/**
 * bar_assistant_machine_copy:
 * @src: a reading
 * @dst: (out caller-allocates): where to copy it; overwritten, not
 *   cleared first
 */
void
bar_assistant_machine_copy(const BarAssistantMachine *src,
                           BarAssistantMachine *dst)
{
	*dst = *src;
	dst->gpu_kind = g_strdup(src->gpu_kind);
	dst->top      = g_strdupv(src->top);
}

gboolean
bar_assistant_parse_nvidia_smi(const gchar *text, guint64 *total,
                               guint64 *used)
{
	g_auto(GStrv) lines = NULL;
	guint i, found;

	*total = 0;
	*used  = 0;
	if (text == NULL)
		return FALSE;

	/* One `total, used' line per GPU, in MiB.  ollama splits a model
	   across GPUs, so the sum is what it can use. */
	lines = g_strsplit(text, "\n", 64);
	found = 0;
	for (i = 0; lines[i] != NULL; i++) {
		gchar *end = NULL;
		guint64 t, u;
		const gchar *p = lines[i];

		while (g_ascii_isspace(*p))
			p++;
		if (!g_ascii_isdigit(*p))
			continue;
		t = g_ascii_strtoull(p, &end, 10);
		if (end == NULL || *end != ',')
			continue;
		p = end + 1;
		while (g_ascii_isspace(*p))
			p++;
		if (!g_ascii_isdigit(*p))
			continue;
		u = g_ascii_strtoull(p, NULL, 10);
		if (u > t)
			u = t;
		*total += t * 1024 * 1024;
		*used  += u * 1024 * 1024;
		found++;
	}
	return found > 0;
}

/* A small sysfs/procfs file as a number; 0 when absent. */
static guint64
read_number_file(const gchar *path)
{
	g_autofree gchar *text = NULL;
	gsize len = 0;

	if (!g_file_get_contents(path, &text, &len, NULL) || len > 64)
		return 0;
	return g_ascii_strtoull(g_strstrip(text), NULL, 10);
}

gboolean
bar_assistant_read_amd_vram(const gchar *sys_root, guint64 *total,
                            guint64 *used)
{
	g_autofree gchar *drm = NULL;
	GDir *dir;
	const gchar *name;
	gboolean found = FALSE;

	*total = 0;
	*used  = 0;
	drm = g_build_filename((sys_root != NULL) ? sys_root : "/sys",
	                       "class", "drm", NULL);
	dir = g_dir_open(drm, 0, NULL);
	if (dir == NULL)
		return FALSE;

	/*
	 * `cardN' only: `card1-DP-1' and friends are the card's
	 * connectors and carry no memory.  A card with less than
	 * BAR_ASSISTANT_MIN_VRAM is an integrated GPU's carve-out, where
	 * nothing will run a model, and counting it would make every
	 * model on a laptop look too big.
	 */
	while ((name = g_dir_read_name(dir)) != NULL) {
		g_autofree gchar *tpath = NULL;
		g_autofree gchar *upath = NULL;
		const gchar *p;
		guint64 t, u;
		gboolean digits = TRUE;

		if (!g_str_has_prefix(name, "card") || name[4] == '\0')
			continue;
		for (p = name + 4; *p != '\0'; p++)
			if (!g_ascii_isdigit(*p))
				digits = FALSE;
		if (!digits)
			continue;

		tpath = g_build_filename(drm, name, "device",
		                         "mem_info_vram_total", NULL);
		upath = g_build_filename(drm, name, "device",
		                         "mem_info_vram_used", NULL);
		t = read_number_file(tpath);
		u = read_number_file(upath);
		if (t < BAR_ASSISTANT_MIN_VRAM)
			continue;
		*total += t;
		*used  += MIN(u, t);
		found = TRUE;
	}
	g_dir_close(dir);
	return found;
}

static void
read_meminfo(const gchar *proc_root, BarAssistantMachine *out)
{
	g_autofree gchar *path = NULL;
	g_autofree gchar *text = NULL;
	g_auto(GStrv) lines = NULL;
	guint i;

	path = g_build_filename(proc_root, "meminfo", NULL);
	if (!g_file_get_contents(path, &text, NULL, NULL))
		return;
	lines = g_strsplit(text, "\n", 128);
	for (i = 0; lines[i] != NULL; i++) {
		guint64 *slot = NULL;
		const gchar *p;

		if (g_str_has_prefix(lines[i], "MemTotal:"))
			slot = &out->mem_total;
		else if (g_str_has_prefix(lines[i], "MemAvailable:"))
			slot = &out->mem_avail;
		if (slot == NULL)
			continue;
		p = strchr(lines[i], ':') + 1;
		while (g_ascii_isspace(*p))
			p++;
		/* Always kB, whatever the line says. */
		*slot = g_ascii_strtoull(p, NULL, 10) * 1024;
	}
}

static void
read_loadavg(const gchar *proc_root, BarAssistantMachine *out)
{
	g_autofree gchar *path = NULL;
	g_autofree gchar *text = NULL;
	gchar *p;
	guint i;

	path = g_build_filename(proc_root, "loadavg", NULL);
	if (!g_file_get_contents(path, &text, NULL, NULL))
		return;
	p = text;
	for (i = 0; i < 3; i++) {
		gchar *end = NULL;

		out->load[i] = g_ascii_strtod(p, &end);
		if (end == p || isnan(out->load[i]) || out->load[i] < 0.0) {
			out->load[i] = 0.0;
			break;
		}
		p = end;
	}
}

static void
read_cores(const gchar *proc_root, BarAssistantMachine *out)
{
	g_autofree gchar *path = NULL;
	g_autofree gchar *text = NULL;
	g_auto(GStrv) lines = NULL;
	guint i;

	path = g_build_filename(proc_root, "stat", NULL);
	if (!g_file_get_contents(path, &text, NULL, NULL))
		return;
	lines = g_strsplit(text, "\n", 4096);
	for (i = 0; lines[i] != NULL; i++)
		if (g_str_has_prefix(lines[i], "cpu") &&
		    g_ascii_isdigit(lines[i][3]))
			out->cores++;
}

/* The nearest existing directory at or above @path. */
static gchar *
existing_parent(const gchar *path)
{
	gchar *dir;

	dir = g_strdup(path);
	while (!g_file_test(dir, G_FILE_TEST_IS_DIR)) {
		gchar *up = g_path_get_dirname(dir);

		if (strcmp(up, dir) == 0) {
			g_free(up);
			break;
		}
		g_free(dir);
		dir = up;
	}
	return dir;
}

typedef struct {
	gchar   *name;
	guint64  rss;
} ProcEntry;

static gint
proc_entry_cmp(gconstpointer a, gconstpointer b)
{
	const ProcEntry *pa = *(ProcEntry * const *)a;
	const ProcEntry *pb = *(ProcEntry * const *)b;

	if (pa->rss == pb->rss)
		return g_strcmp0(pa->name, pb->name);
	return (pa->rss > pb->rss) ? -1 : 1;
}

static void
proc_entry_free(gpointer data)
{
	ProcEntry *e = data;

	g_free(e->name);
	g_free(e);
}

gchar **
bar_assistant_top_processes(const gchar *proc_root, guint n)
{
	g_autoptr(GPtrArray) entries = NULL;
	GPtrArray *out;
	GDir *dir;
	const gchar *name;
	glong page;
	guint scanned = 0, i;

	out = g_ptr_array_new();
	dir = g_dir_open((proc_root != NULL) ? proc_root : "/proc", 0, NULL);
	if (dir == NULL || n == 0) {
		if (dir != NULL)
			g_dir_close(dir);
		g_ptr_array_add(out, NULL);
		return (gchar **)g_ptr_array_free(out, FALSE);
	}

	page = sysconf(_SC_PAGESIZE);
	if (page <= 0)
		page = 4096;

	entries = g_ptr_array_new_with_free_func(proc_entry_free);
	while ((name = g_dir_read_name(dir)) != NULL &&
	       scanned < PROC_SCAN_MAX) {
		g_autofree gchar *statm_path = NULL;
		g_autofree gchar *comm_path = NULL;
		g_autofree gchar *statm = NULL;
		g_autofree gchar *comm = NULL;
		const gchar *p;
		gchar *end = NULL;
		guint64 resident;
		ProcEntry *e;
		gboolean digits = TRUE;

		for (p = name; *p != '\0'; p++)
			if (!g_ascii_isdigit(*p))
				digits = FALSE;
		if (!digits)
			continue;
		scanned++;

		statm_path = g_build_filename(proc_root, name, "statm", NULL);
		comm_path  = g_build_filename(proc_root, name, "comm", NULL);
		if (!g_file_get_contents(statm_path, &statm, NULL, NULL) ||
		    !g_file_get_contents(comm_path, &comm, NULL, NULL))
			continue;

		/* statm: size resident shared ... in pages. */
		p = statm;
		g_ascii_strtoull(p, &end, 10);
		if (end == NULL || end == p)
			continue;
		resident = g_ascii_strtoull(end, NULL, 10);
		if (resident == 0)
			continue;   /* kernel threads */

		e = g_new0(ProcEntry, 1);
		e->name = clean_text(g_strstrip(comm), 32);
		e->rss  = resident * (guint64)page;
		if (e->name[0] == '\0') {
			proc_entry_free(e);
			continue;
		}
		g_ptr_array_add(entries, e);
	}
	g_dir_close(dir);

	g_ptr_array_sort(entries, proc_entry_cmp);
	for (i = 0; i < entries->len && out->len < n; i++) {
		ProcEntry *e = g_ptr_array_index(entries, i);

		add_unique(out, e->name);
	}
	g_ptr_array_add(out, NULL);
	return (gchar **)g_ptr_array_free(out, FALSE);
}

void
bar_assistant_read_machine(const gchar *proc_root, const gchar *sys_root,
                           const gchar *nvidia_text,
                           const gchar *models_dir,
                           BarAssistantMachine *out)
{
	guint64 t = 0, u = 0;
	const gchar *proc;

	g_return_if_fail(out != NULL);

	memset(out, 0, sizeof(*out));
	proc = (proc_root != NULL) ? proc_root : "/proc";

	read_meminfo(proc, out);
	read_loadavg(proc, out);
	read_cores(proc, out);
	out->top = bar_assistant_top_processes(proc, 5);

	/* NVIDIA first: on a box with both, the discrete NVIDIA card is
	   where ollama puts a model. */
	if (bar_assistant_parse_nvidia_smi(nvidia_text, &t, &u) &&
	    t >= BAR_ASSISTANT_MIN_VRAM) {
		out->gpu_kind   = g_strdup("NVIDIA");
		out->vram_total = t;
		out->vram_used  = u;
	} else if (bar_assistant_read_amd_vram(sys_root, &t, &u)) {
		out->gpu_kind   = g_strdup("AMD");
		out->vram_total = t;
		out->vram_used  = u;
	}

	if (models_dir != NULL && models_dir[0] != '\0') {
		g_autofree gchar *dir = existing_parent(models_dir);
		struct statvfs st;

		if (statvfs(dir, &st) == 0) {
			out->disk_free  = (guint64)st.f_bavail *
			                  (guint64)st.f_frsize;
			out->disk_total = (guint64)st.f_blocks *
			                  (guint64)st.f_frsize;
		}
	}
}

BarAssistantFit
bar_assistant_fit(guint64 model_size, const BarAssistantMachine *machine)
{
	guint64 need, vram_free;

	if (machine == NULL || model_size == 0)
		return BAR_ASSISTANT_FIT_UNKNOWN;

	/* Weights plus a fifth again for the context cache and the
	   runtime: a model exactly the size of free memory will not run
	   with any context worth having. */
	need = model_size + model_size / 5;

	if (machine->gpu_kind != NULL &&
	    machine->vram_total >= BAR_ASSISTANT_MIN_VRAM) {
		vram_free = (machine->vram_total > machine->vram_used)
			? machine->vram_total - machine->vram_used : 0;
		if (need <= vram_free)
			return BAR_ASSISTANT_FIT_COMFORTABLE;
		/* ollama spills layers it cannot place into RAM: it runs,
		   at CPU speed for those layers. */
		if (model_size <= vram_free + machine->mem_avail)
			return BAR_ASSISTANT_FIT_TIGHT;
		return BAR_ASSISTANT_FIT_TOO_BIG;
	}

	if (machine->mem_avail == 0)
		return BAR_ASSISTANT_FIT_UNKNOWN;
	if (need <= machine->mem_avail)
		return BAR_ASSISTANT_FIT_COMFORTABLE;
	if (model_size <= machine->mem_avail)
		return BAR_ASSISTANT_FIT_TIGHT;
	return BAR_ASSISTANT_FIT_TOO_BIG;
}

/**
 * bar_assistant_fit_label:
 * @fit: a verdict
 *
 * Returns: (transfer none): the badge text
 */
const gchar *
bar_assistant_fit_label(BarAssistantFit fit)
{
	switch (fit) {
	case BAR_ASSISTANT_FIT_COMFORTABLE: return "comfortable";
	case BAR_ASSISTANT_FIT_TIGHT:       return "tight";
	case BAR_ASSISTANT_FIT_TOO_BIG:     return "too big";
	case BAR_ASSISTANT_FIT_UNKNOWN:
	default:                            return "unknown";
	}
}

gchar *
bar_assistant_format_bytes(guint64 bytes)
{
	static const gchar *units[] = { "kB", "MB", "GB", "TB", "PB" };
	gchar buf[G_ASCII_DTOSTR_BUF_SIZE];
	gdouble v;
	guint u;

	if (bytes < 1000)
		return g_strdup_printf("%" G_GUINT64_FORMAT " B", bytes);
	v = (gdouble)bytes / 1000.0;
	u = 0;
	while (v >= 1000.0 && u + 1 < G_N_ELEMENTS(units)) {
		v /= 1000.0;
		u++;
	}
	g_ascii_formatd(buf, sizeof(buf), "%.1f", v);
	return g_strdup_printf("%s %s", buf, units[u]);
}

gchar *
bar_assistant_machine_summary(const BarAssistantMachine *machine,
                              const gchar * const *loaded)
{
	GString *out;
	gchar l1[G_ASCII_DTOSTR_BUF_SIZE];
	gchar l5[G_ASCII_DTOSTR_BUF_SIZE];
	gchar l15[G_ASCII_DTOSTR_BUF_SIZE];

	g_return_val_if_fail(machine != NULL, NULL);

	out = g_string_new("Live system info, read moments ago: ");

	g_ascii_formatd(l1, sizeof(l1), "%.2f", machine->load[0]);
	g_ascii_formatd(l5, sizeof(l5), "%.2f", machine->load[1]);
	g_ascii_formatd(l15, sizeof(l15), "%.2f", machine->load[2]);
	g_string_append_printf(out, "%u CPU threads, load %s %s %s",
	                       machine->cores, l1, l5, l15);

	if (machine->mem_total > 0) {
		g_autofree gchar *used = NULL;
		g_autofree gchar *total = NULL;
		g_autofree gchar *avail = NULL;

		used  = bar_assistant_format_bytes(
			(machine->mem_total > machine->mem_avail)
			? machine->mem_total - machine->mem_avail : 0);
		total = bar_assistant_format_bytes(machine->mem_total);
		avail = bar_assistant_format_bytes(machine->mem_avail);
		g_string_append_printf(out,
			"; memory %s used of %s (%s available)",
			used, total, avail);
	}

	if (machine->gpu_kind != NULL && machine->vram_total > 0) {
		g_autofree gchar *used = NULL;
		g_autofree gchar *total = NULL;

		used  = bar_assistant_format_bytes(machine->vram_used);
		total = bar_assistant_format_bytes(machine->vram_total);
		g_string_append_printf(out, "; %s GPU with %s of %s VRAM in use",
		                       machine->gpu_kind, used, total);
	}

	if (machine->disk_total > 0) {
		g_autofree gchar *free_text = NULL;

		free_text = bar_assistant_format_bytes(machine->disk_free);
		g_string_append_printf(out, "; %s free on the model disk",
		                       free_text);
	}

	if (machine->top != NULL && machine->top[0] != NULL) {
		g_autofree gchar *names = NULL;

		names = g_strjoinv(", ", machine->top);
		g_string_append_printf(out,
			"; largest processes by memory: %s", names);
	}

	if (loaded != NULL) {
		if (loaded[0] == NULL) {
			g_string_append(out, "; no local models loaded");
		} else {
			g_autofree gchar *names = NULL;

			names = g_strjoinv(", ", (gchar **)loaded);
			g_string_append_printf(out, "; loaded models: %s",
			                       names);
		}
	}

	g_string_append_c(out, '.');
	return g_string_free(out, FALSE);
}

/* ── The approval card ────────────────────────────────────────────── */

/**
 * bar_assistant_approval_arm:
 * @self: a card
 * @op: what is to be approved
 * @model: (nullable): the model it concerns
 * @detail: (nullable): what the card says
 * @now: monotonic time
 *
 * Shows a card, replacing any already showing.  Arming never does
 * anything else: the operation runs only from
 * bar_assistant_approval_take().
 */
void
bar_assistant_approval_arm(BarAssistantApproval *self, BarAssistantOp op,
                           const gchar *model, const gchar *detail,
                           gint64 now)
{
	g_return_if_fail(self != NULL);

	bar_assistant_approval_clear(self);
	self->op       = op;
	self->model    = g_strdup(model);
	self->detail   = g_strdup(detail);
	self->armed_at = now;
}

/**
 * bar_assistant_approval_clear:
 * @self: a card
 *
 * Cancel: forget the card.
 */
void
bar_assistant_approval_clear(BarAssistantApproval *self)
{
	g_return_if_fail(self != NULL);

	g_clear_pointer(&self->model, g_free);
	g_clear_pointer(&self->detail, g_free);
	self->op = BAR_ASSISTANT_OP_NONE;
	self->armed_at = 0;
}

gboolean
bar_assistant_approval_live(BarAssistantApproval *self, gint64 now)
{
	g_return_val_if_fail(self != NULL, FALSE);

	if (self->op == BAR_ASSISTANT_OP_NONE)
		return FALSE;
	/* A clock going backwards is not a reason to keep consent alive
	   either. */
	if (now < self->armed_at ||
	    now - self->armed_at >= BAR_ASSISTANT_APPROVAL_TTL) {
		bar_assistant_approval_clear(self);
		return FALSE;
	}
	return TRUE;
}

BarAssistantOp
bar_assistant_approval_take(BarAssistantApproval *self, gint64 now,
                            gchar **model)
{
	BarAssistantOp op;

	if (model != NULL)
		*model = NULL;
	g_return_val_if_fail(self != NULL, BAR_ASSISTANT_OP_NONE);

	if (!bar_assistant_approval_live(self, now))
		return BAR_ASSISTANT_OP_NONE;
	op = self->op;
	if (model != NULL)
		*model = g_steal_pointer(&self->model);
	bar_assistant_approval_clear(self);
	return op;
}

/* ── The bar indicator ────────────────────────────────────────────── */

const gchar *
bar_assistant_look_color_key(BarAssistantLook look)
{
	switch (look) {
	case BAR_ASSISTANT_LOOK_THINKING:  return "color-thinking";
	case BAR_ASSISTANT_LOOK_STREAMING: return "color-streaming";
	case BAR_ASSISTANT_LOOK_ERROR:     return "color-error";
	case BAR_ASSISTANT_LOOK_IDLE:
	default:                           return "color-idle";
	}
}

/**
 * bar_assistant_look_name:
 * @look: a state
 *
 * Returns: (transfer none): its name, for tooltips and the signature
 */
const gchar *
bar_assistant_look_name(BarAssistantLook look)
{
	switch (look) {
	case BAR_ASSISTANT_LOOK_THINKING:  return "thinking";
	case BAR_ASSISTANT_LOOK_STREAMING: return "streaming";
	case BAR_ASSISTANT_LOOK_ERROR:     return "error";
	case BAR_ASSISTANT_LOOK_IDLE:
	default:                           return "idle";
	}
}

void
bar_assistant_wave(const gdouble *samples, guint n_samples, guint n_bars,
                   gdouble phase, gboolean animate, gdouble *out)
{
	guint i;

	g_return_if_fail(out != NULL || n_bars == 0);

	for (i = 0; i < n_bars; i++) {
		gdouble v = 0.0;
		gdouble h;
		gint idx;

		/* Right-aligned: the newest sample is the rightmost bar. */
		idx = (gint)n_samples - (gint)n_bars + (gint)i;
		if (samples != NULL && idx >= 0)
			v = samples[idx];
		if (isnan(v) || v < 0.0)
			v = 0.0;
		if (v > 1.0)
			v = 1.0;

		h = BAR_ASSISTANT_WAVE_MIN + (1.0 - BAR_ASSISTANT_WAVE_MIN) * v;

		if (animate) {
			/*
			 * A ripple travelling left to right.  It modulates
			 * the reading rather than replacing it, so a fast
			 * stream still looks fuller than a trickle; and it
			 * has a floor of its own, so "thinking" with nothing
			 * measured yet still visibly moves.
			 */
			gdouble ripple = 0.5 + 0.5 * sin(phase - (gdouble)i * 0.9);

			h = h * (0.6 + 0.4 * ripple);
			h = MAX(h, BAR_ASSISTANT_WAVE_MIN + 0.3 * ripple);
		}

		out[i] = CLAMP(h, BAR_ASSISTANT_WAVE_MIN, 1.0);
	}
}

/* ── Handing a conversation to cmacs ──────────────────────────────── */

/* ` "text"' or ` nil'; FALSE when not UTF-8. */
static gboolean
append_elisp_string(GString *out, const gchar *text)
{
	g_autofree gchar *quoted = NULL;

	if (text == NULL || text[0] == '\0') {
		g_string_append(out, " nil");
		return TRUE;
	}
	if (!g_utf8_validate(text, -1, NULL))
		return FALSE;
	quoted = bar_elisp_quote(text);
	g_string_append_printf(out, " \"%s\"", quoted);
	return TRUE;
}

gchar *
bar_assistant_cmacs_open_cmdline(const gchar *emacsctl,
                                 const gchar *provider,
                                 const gchar *model,
                                 const gchar *transcript)
{
	g_autoptr(GString) form = NULL;
	g_autofree gchar *qctl = NULL;
	g_autofree gchar *qform = NULL;
	const gchar *tail;

	tail = transcript;
	if (tail != NULL && strlen(tail) > BAR_ASSISTANT_MAX_HANDOFF) {
		/* The end, on a character boundary. */
		tail = tail + strlen(tail) - BAR_ASSISTANT_MAX_HANDOFF;
		while (*tail != '\0' && ((guchar)*tail & 0xC0) == 0x80)
			tail++;
	}

	form = g_string_new("(cmacs-gowl-bar-assistant-open-in-chat");
	if (!append_elisp_string(form, provider) ||
	    !append_elisp_string(form, model) ||
	    !append_elisp_string(form, tail))
		return NULL;
	g_string_append_c(form, ')');

	qctl  = g_shell_quote((emacsctl != NULL && emacsctl[0] != '\0')
	                      ? emacsctl : "emacsctl");
	qform = g_shell_quote(form->str);
	return g_strdup_printf("%s eval %s", qctl, qform);
}
