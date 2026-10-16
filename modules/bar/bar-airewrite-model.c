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
 * The pure half of the `ai-rewrite' widget.  See bar-airewrite-model.h
 * for the contract of each function; the comments here are about how.
 */

#include "bar-airewrite-model.h"

#include <string.h>

#include "yaml-glib.h"

/* ----------------------------------------------------------------
 * Actions
 * ---------------------------------------------------------------- */

/* Every built-in prompt ends the same way.  Models wrap their answers
   in preambles and fences unless told, in so many words, not to; and
   an answer that is pasted over the user's text cannot carry either. */
#define OUTPUT_ONLY \
	" Output ONLY the resulting text: no preamble, no notes on what" \
	" you changed, no quotes or code fences around it."

/* Only lower-case letters and digits, minus the four the panel already
   uses.  Upper case is Shift+letter, which means "and paste". */
gboolean
bar_airewrite_key_usable(gchar key)
{
	if (!((key >= 'a' && key <= 'z') || (key >= '0' && key <= '9')))
		return FALSE;
	return key != 'j' && key != 'k' && key != 'y' && key != 'v';
}

/* A field that is NULL or empty is stored as NULL, so "has a system
   prompt" is one test everywhere instead of two. */
static gchar *
dup_or_null(const gchar *text)
{
	if (text == NULL || text[0] == '\0')
		return NULL;
	return g_strdup(text);
}

/**
 * bar_airewrite_action_new:
 * @key: the hotkey, or 0
 * @id: the identifier
 * @label: the row's text
 * @system: (nullable): the system prompt
 * @instruction: (nullable): the instruction
 *
 * A new action that previews its answer as a diff and asks for no
 * input; set the other fields directly.
 *
 * Returns: (transfer full): the action
 */
BarAirewriteAction *
bar_airewrite_action_new(
	gchar        key,
	const gchar *id,
	const gchar *label,
	const gchar *system,
	const gchar *instruction
){
	BarAirewriteAction *self;

	self = g_new0(BarAirewriteAction, 1);
	self->key         = bar_airewrite_key_usable(key) ? key : '\0';
	self->id          = g_strdup((id != NULL) ? id : "");
	self->label       = g_strdup((label != NULL) ? label : "");
	self->system      = dup_or_null(system);
	self->instruction = g_strdup((instruction != NULL) ? instruction : "");
	self->preview     = TRUE;
	self->diff        = TRUE;
	return self;
}

/**
 * bar_airewrite_action_copy:
 * @self: an action
 *
 * Returns: (transfer full): a deep copy
 */
BarAirewriteAction *
bar_airewrite_action_copy(const BarAirewriteAction *self)
{
	BarAirewriteAction *copy;

	copy = bar_airewrite_action_new(self->key, self->id, self->label,
	                                self->system, self->instruction);
	copy->input    = dup_or_null(self->input);
	copy->provider = dup_or_null(self->provider);
	copy->model    = dup_or_null(self->model);
	copy->preview  = self->preview;
	copy->diff     = self->diff;
	return copy;
}

/**
 * bar_airewrite_action_free:
 * @self: (nullable): an action
 */
void
bar_airewrite_action_free(BarAirewriteAction *self)
{
	if (self == NULL)
		return;
	g_free(self->id);
	g_free(self->label);
	g_free(self->system);
	g_free(self->instruction);
	g_free(self->input);
	g_free(self->provider);
	g_free(self->model);
	g_free(self);
}

static GPtrArray *
action_array_new(void)
{
	return g_ptr_array_new_with_free_func(
		(GDestroyNotify)bar_airewrite_action_free);
}

/**
 * bar_airewrite_builtin_actions:
 *
 * Returns: (transfer full) (element-type BarAirewriteAction): the
 *   standalone defaults
 */
GPtrArray *
bar_airewrite_builtin_actions(void)
{
	GPtrArray *out;
	BarAirewriteAction *a;

	out = action_array_new();

	/* First, because it is the one people reach for, and the one
	   whose whole value is in changing NOTHING else. */
	g_ptr_array_add(out, bar_airewrite_action_new('f', "proofread",
		"Fix spelling & grammar",
		"You are a careful copy editor.  Correct spelling, grammar,"
		" punctuation and obvious typos in the text you are given,"
		" and change NOTHING else: keep the author's wording, voice,"
		" register, formatting, line breaks, markup, code, names and"
		" technical terms exactly as they are.  Do not rephrase for"
		" style, do not add or remove content, do not translate.  If"
		" the text is already correct, return it unchanged." OUTPUT_ONLY,
		"Correct the spelling, grammar and punctuation of the"
		" following, changing nothing else."));

	g_ptr_array_add(out, bar_airewrite_action_new('i', "improve",
		"Improve",
		"You rewrite text so it says the same thing better.  Preserve"
		" meaning, facts, names and technical content exactly.  Make it"
		" clearer, tighter and more direct; keep the author's register"
		" and the source's format (prose stays prose, a list stays a"
		" list)." OUTPUT_ONLY,
		"Improve the following text."));

	g_ptr_array_add(out, bar_airewrite_action_new('s', "shorten",
		"Shorten",
		"You shorten text without losing anything that matters.  Keep"
		" every fact, name, number and decision, and the author's voice;"
		" cut padding, repetition and hedging." OUTPUT_ONLY,
		"Shorten the following text."));

	g_ptr_array_add(out, bar_airewrite_action_new('e', "translate-en",
		"Translate to English",
		"You are a translator.  Translate the text you are given into"
		" natural English, keeping its tone, formatting, names and any"
		" code exactly as they are.  Text that is already English is"
		" returned unchanged." OUTPUT_ONLY,
		"Translate the following text into English."));

	a = bar_airewrite_action_new('c', "custom", "Custom...",
		"You rewrite text exactly as the user instructs, changing"
		" nothing the instruction does not ask you to change."
		OUTPUT_ONLY,
		"Rewrite the following text.  Instruction: {input}");
	a->input = g_strdup("How should it be rewritten?");
	g_ptr_array_add(out, a);

	return out;
}

/* ----------------------------------------------------------------
 * The pushed line format
 * ---------------------------------------------------------------- */

#define SPEC_HEADER  "#ai-rewrite-actions 1"
#define SPEC_FIELDS  9

/**
 * bar_airewrite_escape:
 * @text: (nullable): a field
 *
 * Returns: (transfer full): the escaped field
 */
gchar *
bar_airewrite_escape(const gchar *text)
{
	GString *out;
	const gchar *p;

	out = g_string_new(NULL);
	if (text == NULL)
		return g_string_free(out, FALSE);
	for (p = text; *p != '\0'; p++) {
		switch (*p) {
		case '\\': g_string_append(out, "\\\\"); break;
		case '\t': g_string_append(out, "\\t");  break;
		case '\n': g_string_append(out, "\\n");  break;
		case '\r': g_string_append(out, "\\r");  break;
		default:   g_string_append_c(out, *p);   break;
		}
	}
	return g_string_free(out, FALSE);
}

/**
 * bar_airewrite_unescape:
 * @text: an escaped field
 *
 * Returns: (transfer full): the field
 */
gchar *
bar_airewrite_unescape(const gchar *text)
{
	GString *out;
	const gchar *p;

	out = g_string_new(NULL);
	if (text == NULL)
		return g_string_free(out, FALSE);
	for (p = text; *p != '\0'; p++) {
		if (*p != '\\') {
			g_string_append_c(out, *p);
			continue;
		}
		if (p[1] == '\0') {
			/* A lone backslash at the end is a backslash. */
			g_string_append_c(out, '\\');
			break;
		}
		p++;
		switch (*p) {
		case 't': g_string_append_c(out, '\t'); break;
		case 'n': g_string_append_c(out, '\n'); break;
		case 'r': g_string_append_c(out, '\r'); break;
		default:  g_string_append_c(out, *p);   break;
		}
	}
	return g_string_free(out, FALSE);
}

/**
 * bar_airewrite_actions_serialize:
 * @actions: (element-type BarAirewriteAction): a list
 *
 * Returns: (transfer full): the text
 */
gchar *
bar_airewrite_actions_serialize(GPtrArray *actions)
{
	GString *out;
	guint i;

	out = g_string_new(SPEC_HEADER "\n");
	for (i = 0; actions != NULL && i < actions->len; i++) {
		const BarAirewriteAction *a = g_ptr_array_index(actions, i);
		const gchar *fields[SPEC_FIELDS];
		gchar key[2];
		gchar flags[3];
		guint f, nf = 0;

		key[0] = a->key;
		key[1] = '\0';
		if (a->preview)
			flags[nf++] = 'p';
		if (a->diff)
			flags[nf++] = 'd';
		flags[nf] = '\0';

		fields[0] = key;
		fields[1] = a->id;
		fields[2] = a->label;
		fields[3] = flags;
		fields[4] = a->input;
		fields[5] = a->provider;
		fields[6] = a->model;
		fields[7] = a->system;
		fields[8] = a->instruction;

		for (f = 0; f < SPEC_FIELDS; f++) {
			g_autofree gchar *esc = bar_airewrite_escape(fields[f]);

			if (f > 0)
				g_string_append_c(out, '\t');
			g_string_append(out, esc);
		}
		g_string_append_c(out, '\n');
	}
	return g_string_free(out, FALSE);
}

/* Cut a field at the cap, on a character boundary.  The input is
   already known to be valid UTF-8. */
static gchar *
cap_field(gchar *field)
{
	const gchar *end;

	if (field == NULL || strlen(field) <= BAR_AIREWRITE_MAX_FIELD)
		return field;
	end = g_utf8_find_prev_char(field, field + BAR_AIREWRITE_MAX_FIELD + 1);
	if (end == NULL)
		end = field;
	field[end - field] = '\0';
	return field;
}

/**
 * bar_airewrite_actions_parse:
 * @spec: (nullable): the value of the `actions' setting
 * @n_rejected: (out) (optional): how many lines were refused
 *
 * Returns: (transfer full) (element-type BarAirewriteAction): the actions
 */
GPtrArray *
bar_airewrite_actions_parse(const gchar *spec, guint *n_rejected)
{
	GPtrArray *out;
	g_auto(GStrv) lines = NULL;
	guint rejected = 0;
	guint i;

	out = action_array_new();
	if (n_rejected != NULL)
		*n_rejected = 0;
	if (spec == NULL || spec[0] == '\0')
		return out;

	lines = g_strsplit(spec, "\n", -1);
	for (i = 0; lines[i] != NULL; i++) {
		g_auto(GStrv) raw = NULL;
		gchar *fields[SPEC_FIELDS];
		BarAirewriteAction *a;
		gboolean ok;
		guint f;
		gchar *line = lines[i];
		gsize len = strlen(line);

		/* A CRLF writer is not a reason to lose every action. */
		if (len > 0 && line[len - 1] == '\r')
			line[len - 1] = '\0';
		if (line[0] == '\0' || line[0] == '#')
			continue;

		if (out->len >= BAR_AIREWRITE_MAX_ACTIONS) {
			rejected++;
			continue;
		}

		raw = g_strsplit(line, "\t", -1);
		if (g_strv_length(raw) != SPEC_FIELDS) {
			rejected++;
			continue;
		}

		ok = TRUE;
		for (f = 0; f < SPEC_FIELDS; f++) {
			fields[f] = bar_airewrite_unescape(raw[f]);
			if (!g_utf8_validate(fields[f], -1, NULL))
				ok = FALSE;
		}

		if (!ok || fields[1][0] == '\0' || fields[2][0] == '\0' ||
		    bar_airewrite_actions_find_id(out, fields[1]) != NULL) {
			rejected++;
			for (f = 0; f < SPEC_FIELDS; f++)
				g_free(fields[f]);
			continue;
		}

		/* One character, usable, and not already taken by an
		   earlier line; anything else costs the key, not the row. */
		a = bar_airewrite_action_new(
			(strlen(fields[0]) == 1 &&
			 bar_airewrite_actions_find_key(out, fields[0][0]) == NULL)
				? fields[0][0] : '\0',
			fields[1], fields[2],
			cap_field(fields[7]), cap_field(fields[8]));
		a->preview  = (strchr(fields[3], 'p') != NULL);
		a->diff     = (strchr(fields[3], 'd') != NULL);
		a->input    = dup_or_null(cap_field(fields[4]));
		a->provider = dup_or_null(fields[5]);
		a->model    = dup_or_null(fields[6]);
		g_ptr_array_add(out, a);

		for (f = 0; f < SPEC_FIELDS; f++)
			g_free(fields[f]);
	}

	if (n_rejected != NULL)
		*n_rejected = rejected;
	return out;
}

/* ----------------------------------------------------------------
 * The YAML file
 * ---------------------------------------------------------------- */

/* A scalar member as a string, or NULL.  Numbers and booleans come
   back as written, which is what a `key: 1' needs. */
static const gchar *
yaml_scalar(YamlMapping *map, const gchar *key)
{
	YamlNode *node;

	if (map == NULL || !yaml_mapping_has_member(map, key))
		return NULL;
	node = yaml_mapping_get_member(map, key);
	if (node == NULL || yaml_node_get_node_type(node) != YAML_NODE_SCALAR)
		return NULL;
	return yaml_node_get_scalar(node);
}

static gboolean
yaml_flag(YamlMapping *map, const gchar *key, gboolean fallback)
{
	const gchar *v = yaml_scalar(map, key);

	if (v == NULL)
		return fallback;
	if (g_ascii_strcasecmp(v, "true") == 0 ||
	    g_ascii_strcasecmp(v, "yes") == 0 ||
	    g_ascii_strcasecmp(v, "on") == 0 || strcmp(v, "1") == 0)
		return TRUE;
	if (g_ascii_strcasecmp(v, "false") == 0 ||
	    g_ascii_strcasecmp(v, "no") == 0 ||
	    g_ascii_strcasecmp(v, "off") == 0 || strcmp(v, "0") == 0)
		return FALSE;
	return fallback;
}

/* A string member, made valid UTF-8 and capped.  The file is the
   user's, but a pasted prompt with a stray byte must not take the
   whole list down. */
static gchar *
yaml_text(YamlMapping *map, const gchar *key)
{
	const gchar *v = yaml_scalar(map, key);

	if (v == NULL)
		return NULL;
	return cap_field(g_utf8_make_valid(v, -1));
}

/**
 * bar_airewrite_actions_from_yaml:
 * @yaml: the file's text
 * @replace_builtins: (out) (optional): the `replace-builtins' flag
 * @error: (out) (optional): why it was refused
 *
 * Returns: (transfer full) (nullable): the actions
 */
GPtrArray *
bar_airewrite_actions_from_yaml(
	const gchar  *yaml,
	gboolean     *replace_builtins,
	GError      **error
){
	g_autoptr(YamlParser) parser = NULL;
	YamlNode *root;
	YamlNode *node;
	YamlMapping *map;
	YamlSequence *seq;
	GPtrArray *out;
	guint i, n;

	if (replace_builtins != NULL)
		*replace_builtins = FALSE;

	parser = yaml_parser_new();
	if (!yaml_parser_load_from_data(parser, (yaml != NULL) ? yaml : "",
	                                -1, error))
		return NULL;

	out = action_array_new();
	root = yaml_parser_get_root(parser);
	if (root == NULL || yaml_node_get_node_type(root) == YAML_NODE_NULL)
		return out;

	map = yaml_node_get_mapping(root);
	if (map == NULL) {
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
		                    "the file is not a mapping");
		g_ptr_array_unref(out);
		return NULL;
	}

	if (replace_builtins != NULL)
		*replace_builtins = yaml_flag(map, "replace-builtins", FALSE);

	if (!yaml_mapping_has_member(map, "actions"))
		return out;
	node = yaml_mapping_get_member(map, "actions");
	if (node == NULL || yaml_node_get_node_type(node) == YAML_NODE_NULL)
		return out;
	seq = yaml_node_get_sequence(node);
	if (seq == NULL) {
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
		                    "`actions' must be a list");
		g_ptr_array_unref(out);
		return NULL;
	}

	n = yaml_sequence_get_length(seq);
	for (i = 0; i < n && out->len < BAR_AIREWRITE_MAX_ACTIONS; i++) {
		YamlNode *el = yaml_sequence_get_element(seq, i);
		YamlMapping *entry;
		g_autofree gchar *key = NULL;
		g_autofree gchar *id = NULL;
		g_autofree gchar *label = NULL;
		g_autofree gchar *system = NULL;
		g_autofree gchar *instruction = NULL;
		BarAirewriteAction *a;

		if (el == NULL || yaml_node_get_node_type(el) != YAML_NODE_MAPPING)
			continue;
		entry = yaml_node_get_mapping(el);

		key         = yaml_text(entry, "key");
		id          = yaml_text(entry, "id");
		label       = yaml_text(entry, "label");
		system      = yaml_text(entry, "system");
		instruction = yaml_text(entry, "instruction");

		/* An entry nobody can name or click is a typo, not an
		   action; skip it and keep the rest. */
		if (id == NULL || id[0] == '\0' || label == NULL ||
		    label[0] == '\0' ||
		    bar_airewrite_actions_find_id(out, id) != NULL)
			continue;

		a = bar_airewrite_action_new(
			(key != NULL && strlen(key) == 1 &&
			 bar_airewrite_actions_find_key(out, key[0]) == NULL)
				? key[0] : '\0',
			id, label, system, instruction);
		a->input    = yaml_text(entry, "input");
		a->provider = yaml_text(entry, "provider");
		a->model    = yaml_text(entry, "model");
		a->preview  = yaml_flag(entry, "preview", TRUE);
		a->diff     = yaml_flag(entry, "diff", TRUE);
		if (a->input != NULL && a->input[0] == '\0')
			g_clear_pointer(&a->input, g_free);
		g_ptr_array_add(out, a);
	}
	return out;
}

/**
 * bar_airewrite_actions_find_key:
 * @actions: (element-type BarAirewriteAction): a list
 * @key: a hotkey
 *
 * Returns: (transfer none) (nullable): the action bound to @key
 */
const BarAirewriteAction *
bar_airewrite_actions_find_key(GPtrArray *actions, gchar key)
{
	guint i;

	if (actions == NULL || key == '\0')
		return NULL;
	for (i = 0; i < actions->len; i++) {
		const BarAirewriteAction *a = g_ptr_array_index(actions, i);

		if (a->key == key)
			return a;
	}
	return NULL;
}

/**
 * bar_airewrite_actions_find_id:
 * @actions: (element-type BarAirewriteAction): a list
 * @id: an action id
 *
 * Returns: (transfer none) (nullable): the action called @id
 */
const BarAirewriteAction *
bar_airewrite_actions_find_id(GPtrArray *actions, const gchar *id)
{
	guint i;

	if (actions == NULL || id == NULL)
		return NULL;
	for (i = 0; i < actions->len; i++) {
		const BarAirewriteAction *a = g_ptr_array_index(actions, i);

		if (g_strcmp0(a->id, id) == 0)
			return a;
	}
	return NULL;
}

/**
 * bar_airewrite_actions_merge:
 * @base: (element-type BarAirewriteAction): the list so far
 * @extra: (nullable) (element-type BarAirewriteAction): more actions
 * @replace: %TRUE to drop @base
 *
 * Returns: (transfer full) (element-type BarAirewriteAction): the merge
 */
GPtrArray *
bar_airewrite_actions_merge(GPtrArray *base, GPtrArray *extra,
                            gboolean replace)
{
	GPtrArray *out;
	guint i, j;

	out = action_array_new();

	/* The base, in order, with any same-id extra put in its place. */
	if (!replace && base != NULL) {
		for (i = 0; i < base->len; i++) {
			const BarAirewriteAction *b = g_ptr_array_index(base, i);
			const BarAirewriteAction *over;

			over = bar_airewrite_actions_find_id(extra, b->id);
			g_ptr_array_add(out, bar_airewrite_action_copy(
				(over != NULL) ? over : b));
		}
	}

	/* Then the extras that replaced nothing. */
	for (i = 0; extra != NULL && i < extra->len; i++) {
		const BarAirewriteAction *e = g_ptr_array_index(extra, i);

		if (bar_airewrite_actions_find_id(out, e->id) != NULL)
			continue;
		if (out->len >= BAR_AIREWRITE_MAX_ACTIONS)
			break;
		g_ptr_array_add(out, bar_airewrite_action_copy(e));
	}

	/*
	 * Settle the hotkeys.  An extra action's key wins over a base
	 * one's, so a configured `s: summarize' takes `s' from the
	 * built-in Shorten -- which keeps its row, clickable, and loses
	 * only the key.  Among actions of the same origin the first wins.
	 */
	for (i = 0; i < out->len; i++) {
		BarAirewriteAction *a = g_ptr_array_index(out, i);
		gboolean a_extra;

		if (a->key == '\0')
			continue;
		a_extra = (bar_airewrite_actions_find_id(extra, a->id) != NULL);
		for (j = i + 1; j < out->len; j++) {
			BarAirewriteAction *b = g_ptr_array_index(out, j);
			gboolean b_extra;

			if (b->key != a->key)
				continue;
			b_extra = (bar_airewrite_actions_find_id(extra, b->id)
			           != NULL);
			if (b_extra && !a_extra) {
				a->key = '\0';
				break;
			}
			b->key = '\0';
		}
	}
	return out;
}

/* ----------------------------------------------------------------
 * Prompts and answers
 * ---------------------------------------------------------------- */

/**
 * bar_airewrite_compose_prompt:
 * @action: the action
 * @input: (nullable): what the user typed
 * @text: the text
 *
 * Returns: (transfer full): the prompt
 */
gchar *
bar_airewrite_compose_prompt(const BarAirewriteAction *action,
                             const gchar *input, const gchar *text)
{
	g_auto(GStrv) parts = NULL;
	g_autofree gchar *instruction = NULL;

	parts = g_strsplit((action->instruction != NULL)
	                   ? action->instruction : "", "{input}", -1);
	instruction = g_strjoinv((input != NULL) ? input : "", parts);
	g_strstrip(instruction);

	if (instruction[0] == '\0')
		return g_strdup((text != NULL) ? text : "");
	return g_strdup_printf("%s\n\n%s", instruction,
	                       (text != NULL) ? text : "");
}

/* The whitespace run at the end of @s, as a pointer into it. */
static const gchar *
trailing_space(const gchar *s)
{
	const gchar *end = s + strlen(s);

	while (end > s && g_ascii_isspace(end[-1]))
		end--;
	return end;
}

static gboolean
is_quote_pair(const gchar *s, gsize len)
{
	if (len >= 2 && s[0] == '"' && s[len - 1] == '"')
		return TRUE;
	/* U+201C ... U+201D, three bytes each. */
	if (len >= 6 && strncmp(s, "\xe2\x80\x9c", 3) == 0 &&
	    strncmp(s + len - 3, "\xe2\x80\x9d", 3) == 0)
		return TRUE;
	return FALSE;
}

/**
 * bar_airewrite_clean_answer:
 * @answer: the model's answer
 * @original: the text it rewrote
 *
 * Returns: (transfer full): the cleaned answer
 */
gchar *
bar_airewrite_clean_answer(const gchar *answer, const gchar *original)
{
	g_autofree gchar *work = NULL;
	g_autofree gchar *orig_trim = NULL;
	gchar *body;
	gsize len;

	if (original == NULL)
		original = "";
	work = g_strdup((answer != NULL) ? answer : "");
	body = g_strstrip(work);
	orig_trim = g_strstrip(g_strdup(original));

	/* A fence around the whole answer, when the original had none. */
	if (g_str_has_prefix(body, "```") && !g_str_has_prefix(orig_trim, "```")) {
		gchar *nl = strchr(body, '\n');
		gsize blen;

		blen = strlen(body);
		if (nl != NULL && blen >= 6 &&
		    strcmp(body + blen - 3, "```") == 0) {
			body[blen - 3] = '\0';
			body = nl + 1;
			g_strchomp(body);
		}
	}

	/* A pair of quotes around the whole answer, ditto. */
	len = strlen(body);
	if (is_quote_pair(body, len) &&
	    !is_quote_pair(orig_trim, strlen(orig_trim))) {
		gsize q = (body[0] == '"') ? 1 : 3;

		body[len - q] = '\0';
		body += q;
		g_strstrip(body);
	}

	/* Give back the trailing white space the selection ended with. */
	return g_strconcat(body, trailing_space(original), NULL);
}

/**
 * bar_airewrite_preview:
 * @text: (nullable): the text
 * @max_chars: the longest preview
 * @max_lines: the most lines
 *
 * Returns: (transfer full): the preview
 */
gchar *
bar_airewrite_preview(const gchar *text, guint max_chars, guint max_lines)
{
	g_autofree gchar *valid = NULL;
	GString *out;
	const gchar *p;
	guint chars = 0, lines = 1;
	gboolean cut = FALSE;

	if (text == NULL)
		return g_strdup("");
	valid = g_utf8_make_valid(text, -1);
	out = g_string_new(NULL);
	for (p = valid; *p != '\0'; p = g_utf8_next_char(p)) {
		gunichar c = g_utf8_get_char(p);

		if (chars >= max_chars) {
			cut = TRUE;
			break;
		}
		if (c == '\n') {
			if (lines >= max_lines) {
				cut = TRUE;
				break;
			}
			lines++;
		}
		g_string_append_unichar(out, c);
		chars++;
	}
	if (cut) {
		g_strchomp(out->str);
		g_string_set_size(out, strlen(out->str));
		g_string_append(out, "\xe2\x80\xa6");
	}
	return g_string_free(out, FALSE);
}

/* ----------------------------------------------------------------
 * The word diff
 * ---------------------------------------------------------------- */

typedef enum {
	TOK_WORD,
	TOK_SPACE,
	TOK_OTHER
} TokClass;

static TokClass
classify(gunichar c)
{
	if (g_unichar_isalnum(c) || c == '_' || g_unichar_ismark(c))
		return TOK_WORD;
	if (g_unichar_isspace(c))
		return TOK_SPACE;
	return TOK_OTHER;
}

/**
 * bar_airewrite_tokenize:
 * @text: UTF-8 text
 *
 * Returns: (transfer full) (element-type utf8): the tokens
 */
GPtrArray *
bar_airewrite_tokenize(const gchar *text)
{
	GPtrArray *out;
	const gchar *p, *start;
	TokClass cls = TOK_OTHER;

	out = g_ptr_array_new_with_free_func(g_free);
	if (text == NULL || text[0] == '\0')
		return out;

	start = text;
	p = text;
	while (*p != '\0') {
		TokClass c = classify(g_utf8_get_char(p));
		const gchar *next = g_utf8_next_char(p);

		if (p == start) {
			cls = c;
		} else if (c != cls || cls == TOK_OTHER) {
			/* A run ends where its class does; `other'
			   characters never run together, so `!!' is two
			   tokens and one can change without the other. */
			g_ptr_array_add(out, g_strndup(start,
			                               (gsize)(p - start)));
			start = p;
			cls = c;
		}
		p = next;
	}
	g_ptr_array_add(out, g_strndup(start, (gsize)(p - start)));
	return out;
}

/**
 * bar_airewrite_diff_op_free:
 * @op: (nullable): an op
 */
void
bar_airewrite_diff_op_free(BarAirewriteDiffOp *op)
{
	if (op == NULL)
		return;
	g_free(op->text);
	g_free(op);
}

/* One token's verdict, pointing into the token arrays: building the
   ops by concatenating strings as they arrive is quadratic, and a long
   document diffed on the compositor thread cannot afford that. */
typedef struct {
	BarAirewriteDiffKind  kind;
	const gchar          *text;
} RawOp;

static void
raw_push(GArray *raw, BarAirewriteDiffKind kind, const gchar *text)
{
	RawOp op;

	op.kind = kind;
	op.text = text;
	g_array_append_val(raw, op);
}

static void
ops_take(GPtrArray *ops, BarAirewriteDiffKind kind, GString *text)
{
	BarAirewriteDiffOp *op;

	if (text->len == 0)
		return;
	op = g_new0(BarAirewriteDiffOp, 1);
	op->kind = kind;
	op->text = g_strndup(text->str, text->len);
	g_ptr_array_add(ops, op);
	g_string_truncate(text, 0);
}

static gboolean
all_space(const gchar *s)
{
	for (; *s != '\0'; s = g_utf8_next_char(s))
		if (!g_unichar_isspace(g_utf8_get_char(s)))
			return FALSE;
	return TRUE;
}

/* The largest middle compared word by word: a million cells of a
   guint32 table is 4 MB and a few milliseconds, and it runs on the
   compositor thread. */
#define DIFF_MAX_CELLS 1000000u

/*
 * Group the raw verdicts into ops: inside each changed stretch every
 * deletion comes first, then every insertion; a white-space-only EQUAL
 * between two changes joins both sides.  `the cat' -> `a dog' then
 * reads [-the cat-]{+a dog+} instead of [-the-]{+a+} [-cat-]{+dog+}.
 */
static GPtrArray *
ops_group(GArray *raw)
{
	GPtrArray *out;
	GString *eq, *del, *ins;
	guint i;

	out = g_ptr_array_new_with_free_func(
		(GDestroyNotify)bar_airewrite_diff_op_free);
	eq  = g_string_new(NULL);
	del = g_string_new(NULL);
	ins = g_string_new(NULL);

	for (i = 0; i < raw->len; i++) {
		const RawOp *op = &g_array_index(raw, RawOp, i);

		if (op->kind != BAR_AIREWRITE_DIFF_EQUAL) {
			ops_take(out, BAR_AIREWRITE_DIFF_EQUAL, eq);
			g_string_append((op->kind == BAR_AIREWRITE_DIFF_DELETE)
			                ? del : ins, op->text);
			continue;
		}

		/* Fold white space into the change around it, when there
		   IS a change on both sides of it. */
		if ((del->len > 0 || ins->len > 0) && all_space(op->text) &&
		    i + 1 < raw->len &&
		    g_array_index(raw, RawOp, i + 1).kind
		    != BAR_AIREWRITE_DIFF_EQUAL) {
			g_string_append(del, op->text);
			g_string_append(ins, op->text);
			continue;
		}

		ops_take(out, BAR_AIREWRITE_DIFF_DELETE, del);
		ops_take(out, BAR_AIREWRITE_DIFF_INSERT, ins);
		g_string_append(eq, op->text);
	}
	ops_take(out, BAR_AIREWRITE_DIFF_EQUAL, eq);
	ops_take(out, BAR_AIREWRITE_DIFF_DELETE, del);
	ops_take(out, BAR_AIREWRITE_DIFF_INSERT, ins);

	g_string_free(eq, TRUE);
	g_string_free(del, TRUE);
	g_string_free(ins, TRUE);
	return out;
}

/**
 * bar_airewrite_diff:
 * @before: the old text
 * @after: the new text
 *
 * Returns: (transfer full) (element-type BarAirewriteDiffOp): the ops
 */
GPtrArray *
bar_airewrite_diff(const gchar *before, const gchar *after)
{
	g_autoptr(GPtrArray) a = NULL;
	g_autoptr(GPtrArray) b = NULL;
	g_autoptr(GArray) raw = NULL;
	guint head, tail, n, m, i, j;

	a = bar_airewrite_tokenize(before);
	b = bar_airewrite_tokenize(after);
	raw = g_array_new(FALSE, FALSE, sizeof(RawOp));

	/* The common head and tail first: a proofread changes a word or
	   two in a paragraph, and this leaves a middle of a few tokens. */
	head = 0;
	while (head < a->len && head < b->len &&
	       strcmp(g_ptr_array_index(a, head),
	              g_ptr_array_index(b, head)) == 0)
		head++;
	tail = 0;
	while (tail < a->len - head && tail < b->len - head &&
	       strcmp(g_ptr_array_index(a, a->len - 1 - tail),
	              g_ptr_array_index(b, b->len - 1 - tail)) == 0)
		tail++;

	for (i = 0; i < head; i++)
		raw_push(raw, BAR_AIREWRITE_DIFF_EQUAL,
		         g_ptr_array_index(a, i));

	n = a->len - head - tail;
	m = b->len - head - tail;

	if (n > 0 && m > 0 &&
	    (guint64)(n + 1) * (guint64)(m + 1) <= DIFF_MAX_CELLS) {
		guint32 *L;
		guint w = m + 1;

		/* L[i][j] = LCS length of a[i..] and b[j..] (middle-local
		   indices), filled backwards so the walk below can go
		   forwards and emit deletions before insertions. */
		L = g_new0(guint32, (gsize)(n + 1) * (gsize)w);
		for (i = n; i-- > 0;) {
			for (j = m; j-- > 0;) {
				if (strcmp(g_ptr_array_index(a, head + i),
				           g_ptr_array_index(b, head + j)) == 0)
					L[i * w + j] = L[(i + 1) * w + j + 1] + 1;
				else
					L[i * w + j] = MAX(L[(i + 1) * w + j],
					                   L[i * w + j + 1]);
			}
		}

		i = 0;
		j = 0;
		while (i < n && j < m) {
			const gchar *ta = g_ptr_array_index(a, head + i);
			const gchar *tb = g_ptr_array_index(b, head + j);

			if (strcmp(ta, tb) == 0) {
				raw_push(raw, BAR_AIREWRITE_DIFF_EQUAL, ta);
				i++;
				j++;
			} else if (L[(i + 1) * w + j] >= L[i * w + j + 1]) {
				raw_push(raw, BAR_AIREWRITE_DIFF_DELETE, ta);
				i++;
			} else {
				raw_push(raw, BAR_AIREWRITE_DIFF_INSERT, tb);
				j++;
			}
		}
		for (; i < n; i++)
			raw_push(raw, BAR_AIREWRITE_DIFF_DELETE,
			         g_ptr_array_index(a, head + i));
		for (; j < m; j++)
			raw_push(raw, BAR_AIREWRITE_DIFF_INSERT,
			         g_ptr_array_index(b, head + j));
		g_free(L);
	} else {
		/* Nothing on one side, or too big to compare: one
		   replacement (or one pure insert/delete). */
		for (i = 0; i < n; i++)
			raw_push(raw, BAR_AIREWRITE_DIFF_DELETE,
			         g_ptr_array_index(a, head + i));
		for (j = 0; j < m; j++)
			raw_push(raw, BAR_AIREWRITE_DIFF_INSERT,
			         g_ptr_array_index(b, head + j));
	}

	for (i = a->len - tail; i < a->len; i++)
		raw_push(raw, BAR_AIREWRITE_DIFF_EQUAL,
		         g_ptr_array_index(a, i));

	/* Grouped while the token arrays the raw ops point into are
	   still alive. */
	return ops_group(raw);
}

/**
 * bar_airewrite_diff_to_string:
 * @ops: (element-type BarAirewriteDiffOp): a diff
 *
 * Returns: (transfer full): `[-x-]{+y+}' markup
 */
gchar *
bar_airewrite_diff_to_string(GPtrArray *ops)
{
	GString *out;
	guint i;

	out = g_string_new(NULL);
	for (i = 0; ops != NULL && i < ops->len; i++) {
		const BarAirewriteDiffOp *op = g_ptr_array_index(ops, i);

		switch (op->kind) {
		case BAR_AIREWRITE_DIFF_DELETE:
			g_string_append_printf(out, "[-%s-]", op->text);
			break;
		case BAR_AIREWRITE_DIFF_INSERT:
			g_string_append_printf(out, "{+%s+}", op->text);
			break;
		case BAR_AIREWRITE_DIFF_EQUAL:
		default:
			g_string_append(out, op->text);
			break;
		}
	}
	return g_string_free(out, FALSE);
}

/* ----------------------------------------------------------------
 * Pasting
 * ---------------------------------------------------------------- */

/**
 * bar_airewrite_parse_keys:
 * @spec: a chord
 * @mods: (out): modifier bits
 * @key: (out) (transfer full): the key name
 *
 * Returns: %TRUE when @spec parsed
 */
gboolean
bar_airewrite_parse_keys(const gchar *spec, guint *mods, gchar **key)
{
	g_auto(GStrv) parts = NULL;
	guint i, n, bits = 0;

	*mods = 0;
	*key = NULL;
	if (spec == NULL)
		return FALSE;

	parts = g_strsplit(spec, "+", -1);
	n = g_strv_length(parts);
	if (n == 0)
		return FALSE;

	for (i = 0; i + 1 < n; i++) {
		gchar *p = g_strstrip(parts[i]);

		if (g_ascii_strcasecmp(p, "ctrl") == 0 ||
		    g_ascii_strcasecmp(p, "control") == 0)
			bits |= BAR_AIREWRITE_MOD_CTRL;
		else if (g_ascii_strcasecmp(p, "shift") == 0)
			bits |= BAR_AIREWRITE_MOD_SHIFT;
		else if (g_ascii_strcasecmp(p, "alt") == 0 ||
		         g_ascii_strcasecmp(p, "mod1") == 0)
			bits |= BAR_AIREWRITE_MOD_ALT;
		else if (g_ascii_strcasecmp(p, "super") == 0 ||
		         g_ascii_strcasecmp(p, "logo") == 0 ||
		         g_ascii_strcasecmp(p, "mod4") == 0)
			bits |= BAR_AIREWRITE_MOD_LOGO;
		else
			return FALSE;
	}

	g_strstrip(parts[n - 1]);
	if (parts[n - 1][0] == '\0')
		return FALSE;

	/* One letter is a letter, whatever case it was written in: the
	   keysym for `V' is the shifted one, and Shift is spelled out. */
	if (strlen(parts[n - 1]) == 1)
		*key = g_ascii_strdown(parts[n - 1], -1);
	else
		*key = g_strdup(parts[n - 1]);
	*mods = bits;
	return TRUE;
}

/**
 * bar_airewrite_is_terminal:
 * @app_id: (nullable): an app id
 * @terminal_ids: (nullable): comma-separated app ids
 *
 * Returns: %TRUE when @app_id is listed
 */
gboolean
bar_airewrite_is_terminal(const gchar *app_id, const gchar *terminal_ids)
{
	g_auto(GStrv) ids = NULL;
	guint i;

	if (app_id == NULL || app_id[0] == '\0' || terminal_ids == NULL)
		return FALSE;
	ids = g_strsplit(terminal_ids, ",", -1);
	for (i = 0; ids[i] != NULL; i++) {
		gchar *id = g_strstrip(ids[i]);

		if (id[0] != '\0' && g_ascii_strcasecmp(id, app_id) == 0)
			return TRUE;
	}
	return FALSE;
}

/**
 * bar_airewrite_paste_keys_for:
 * @app_id: (nullable): the target's app id
 * @keys: the ordinary chord
 * @terminal_keys: the terminal chord
 * @terminal_ids: the terminal app ids
 *
 * Returns: (transfer none): the chord to send
 */
const gchar *
bar_airewrite_paste_keys_for(const gchar *app_id, const gchar *keys,
                             const gchar *terminal_keys,
                             const gchar *terminal_ids)
{
	if (terminal_keys != NULL && terminal_keys[0] != '\0' &&
	    bar_airewrite_is_terminal(app_id, terminal_ids))
		return terminal_keys;
	return keys;
}

/**
 * bar_airewrite_paste_step:
 * @paste: the machine
 * @now: monotonic microseconds
 *
 * Returns: what to do
 */
BarAirewritePasteStep
bar_airewrite_paste_step(BarAirewritePaste *paste, gint64 now)
{
	switch (paste->phase) {
	case BAR_AIREWRITE_PASTE_COPIED:
		if (!paste->copied || !paste->inject) {
			/* The clipboard could not be set (nothing to paste),
			   or the user asked only to copy (nothing to put
			   back: the result on the clipboard is the point). */
			paste->phase = BAR_AIREWRITE_PASTE_IDLE;
			return BAR_AIREWRITE_PASTE_FINISH;
		}
		paste->phase      = BAR_AIREWRITE_PASTE_SETTLING;
		paste->settled_at = now;
		return BAR_AIREWRITE_PASTE_INJECT;

	case BAR_AIREWRITE_PASTE_SETTLING:
		if (!paste->restore || !paste->have_previous) {
			paste->phase = BAR_AIREWRITE_PASTE_IDLE;
			return BAR_AIREWRITE_PASTE_FINISH;
		}
		/* The application reads the clipboard when it handles the
		   chord, which is after it gets it; restoring too soon
		   pastes the OLD text.  Hence the wait. */
		if (now - paste->settled_at < paste->restore_delay)
			return BAR_AIREWRITE_PASTE_WAIT;
		paste->phase = BAR_AIREWRITE_PASTE_RESTORING;
		return BAR_AIREWRITE_PASTE_RESTORE;

	case BAR_AIREWRITE_PASTE_IDLE:
	case BAR_AIREWRITE_PASTE_COPYING:
	case BAR_AIREWRITE_PASTE_RESTORING:
	default:
		return BAR_AIREWRITE_PASTE_WAIT;
	}
}

/**
 * bar_airewrite_paste_wake_ms:
 * @paste: the machine
 * @now: monotonic microseconds
 *
 * Returns: milliseconds until the next step, or -1
 */
gint
bar_airewrite_paste_wake_ms(const BarAirewritePaste *paste, gint64 now)
{
	gint64 left;

	switch (paste->phase) {
	case BAR_AIREWRITE_PASTE_COPYING:
	case BAR_AIREWRITE_PASTE_RESTORING:
		/* A worker owns it: look again shortly. */
		return 25;
	case BAR_AIREWRITE_PASTE_COPIED:
		return 1;
	case BAR_AIREWRITE_PASTE_SETTLING:
		left = (paste->settled_at + paste->restore_delay - now) / 1000;
		return (gint)CLAMP(left, 1, 60000);
	case BAR_AIREWRITE_PASTE_IDLE:
	default:
		return -1;
	}
}
