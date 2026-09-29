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
 * gowl-macro-filter.c - Trigger filters.
 *
 * Grammar (keywords are case-insensitive; `&&', `||', `!' work too):
 *
 *   expr   := and ( OR and )*
 *   and    := unary ( AND unary )*
 *   unary  := NOT unary | "(" expr ")" | cond
 *   cond   := FIELD OP VALUE
 *   OP     := "=" | "!=" | "~" | "!~" | "<" | "<=" | ">" | ">="
 *   VALUE  := "double quoted" | 'single quoted' | bare
 *
 * `and' binds tighter than `or', as in C.  `=' and `!=' are glob
 * matches (`*', `?'); `~' and `!~' are regular expressions (GRegex,
 * `(?i)' for case-insensitive); `<' `<=' `>' `>=' compare as numbers
 * when both sides are numbers, else as strings -- so time>=22:00 works.
 * A bare value runs to whitespace or `)'; quote anything else.  A field
 * the event does not have compares as the empty string.
 *
 * Unknown field names are refused at parse time, so a typo is reported
 * when the trigger is installed instead of silently never matching.
 */

#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "gowl-macro"

#include "gowl-macro-filter.h"

#include <string.h>

G_DEFINE_QUARK(gowl-macro-filter-error-quark, gowl_macro_filter_error)

/* Every field the module can supply; see collect_fields() there. */
static const gchar * const known_fields[] = {
	"event",
	"app-id", "title", "floating", "fullscreen", "urgent", "xwayland",
	"focused-app-id", "focused-title",
	"monitor", "layout", "tag", "tags", "clients",
	"arg",
	"time", "hour", "weekday",
	NULL
};

typedef enum {
	NODE_AND,
	NODE_OR,
	NODE_NOT,
	NODE_COND
} NodeKind;

typedef enum {
	OP_GLOB,
	OP_NOT_GLOB,
	OP_REGEX,
	OP_NOT_REGEX,
	OP_LT,
	OP_LE,
	OP_GT,
	OP_GE
} Op;

static const gchar * const op_names[] = {
	"=", "!=", "~", "!~", "<", "<=", ">", ">="
};

typedef struct _Node Node;
struct _Node {
	NodeKind  kind;
	Node     *left;    /* AND/OR: both; NOT: left only */
	Node     *right;
	gchar    *field;   /* COND */
	Op        op;
	gchar    *value;
	GRegex   *regex;   /* OP_REGEX / OP_NOT_REGEX */
};

struct _GowlMacroFilter {
	Node *root;
};

typedef struct {
	const gchar *text;
	gsize        pos;
	GError     **error;
	gboolean     failed;
} Parser;

static void
node_free(Node *n)
{
	if (n == NULL)
		return;
	node_free(n->left);
	node_free(n->right);
	g_free(n->field);
	g_free(n->value);
	if (n->regex != NULL)
		g_regex_unref(n->regex);
	g_free(n);
}

/* Records the first error only; later ones are consequences. */
static void G_GNUC_PRINTF(3, 4)
fail(
	Parser      *p,
	gint         code,
	const gchar *format,
	...
){
	g_autofree gchar *msg = NULL;
	va_list ap;

	if (p->failed)
		return;
	p->failed = TRUE;
	va_start(ap, format);
	msg = g_strdup_vprintf(format, ap);
	va_end(ap);
	g_set_error(p->error, GOWL_MACRO_FILTER_ERROR, code,
	            "filter, at column %u: %s", (guint)p->pos + 1, msg);
}

static void
skip_space(Parser *p)
{
	while (g_ascii_isspace(p->text[p->pos]))
		p->pos++;
}

static gboolean
is_field_char(gchar c)
{
	return g_ascii_isalnum(c) || c == '-' || c == '_';
}

/*
 * Is the word at the cursor the keyword @kw, standing alone (followed
 * by space, `(' or the end)?  Advances past it when it is.
 */
static gboolean
eat_keyword(
	Parser      *p,
	const gchar *kw
){
	gsize n = strlen(kw);
	gchar after;

	if (g_ascii_strncasecmp(p->text + p->pos, kw, n) != 0)
		return FALSE;
	after = p->text[p->pos + n];
	if (after != '\0' && !g_ascii_isspace(after) && after != '(')
		return FALSE;
	p->pos += n;
	return TRUE;
}

static gboolean
eat(
	Parser      *p,
	const gchar *token
){
	gsize n = strlen(token);

	if (strncmp(p->text + p->pos, token, n) != 0)
		return FALSE;
	p->pos += n;
	return TRUE;
}

static Node *parse_or(Parser *p);

/* A value: quoted or bare.  Inside quotes a backslash escapes only the
   quote character and itself; any other backslash is kept, so a regex
   such as '^\[M' means what it says. */
static gchar *
parse_value(Parser *p)
{
	GString *out;
	gchar quote;

	skip_space(p);
	quote = p->text[p->pos];
	out = g_string_new(NULL);
	if (quote == '"' || quote == '\'') {
		p->pos++;
		while (p->text[p->pos] != '\0' && p->text[p->pos] != quote) {
			if (p->text[p->pos] == '\\'
			    && (p->text[p->pos + 1] == quote
			        || p->text[p->pos + 1] == '\\'))
				p->pos++;
			g_string_append_c(out, p->text[p->pos]);
			p->pos++;
		}
		if (p->text[p->pos] != quote) {
			fail(p, GOWL_MACRO_FILTER_ERROR_SYNTAX,
			     "unterminated %c quote", quote);
			return g_string_free(out, TRUE);
		}
		p->pos++;
		return g_string_free(out, FALSE);
	}
	while (p->text[p->pos] != '\0' && !g_ascii_isspace(p->text[p->pos])
	       && p->text[p->pos] != ')' && p->text[p->pos] != '(')
		g_string_append_c(out, p->text[p->pos++]);
	if (out->len == 0) {
		fail(p, GOWL_MACRO_FILTER_ERROR_SYNTAX,
		     "expected a value (quote an empty one: \"\")");
		return g_string_free(out, TRUE);
	}
	return g_string_free(out, FALSE);
}

static Node *
parse_cond(Parser *p)
{
	g_autofree gchar *field = NULL;
	gsize start;
	Node *n;
	Op op;

	skip_space(p);
	start = p->pos;
	while (is_field_char(p->text[p->pos]))
		p->pos++;
	if (p->pos == start) {
		if (p->text[p->pos] == '\0')
			fail(p, GOWL_MACRO_FILTER_ERROR_SYNTAX,
			     "expected a condition, found the end");
		else
			fail(p, GOWL_MACRO_FILTER_ERROR_SYNTAX,
			     "expected a field name, found `%c'", p->text[p->pos]);
		return NULL;
	}
	field = g_ascii_strdown(p->text + start, (gssize)(p->pos - start));
	if (!g_strv_contains(known_fields, field)) {
		g_autofree gchar *all = g_strjoinv(", ", (gchar **)known_fields);

		p->pos = start;
		fail(p, GOWL_MACRO_FILTER_ERROR_FIELD,
		     "unknown field `%s' (known: %s)", field, all);
		return NULL;
	}

	/* Longest operators first */
	skip_space(p);
	if (eat(p, "!="))
		op = OP_NOT_GLOB;
	else if (eat(p, "!~"))
		op = OP_NOT_REGEX;
	else if (eat(p, "<="))
		op = OP_LE;
	else if (eat(p, ">="))
		op = OP_GE;
	else if (eat(p, "=="))
		op = OP_GLOB;
	else if (eat(p, "="))
		op = OP_GLOB;
	else if (eat(p, "~"))
		op = OP_REGEX;
	else if (eat(p, "<"))
		op = OP_LT;
	else if (eat(p, ">"))
		op = OP_GT;
	else {
		fail(p, GOWL_MACRO_FILTER_ERROR_SYNTAX,
		     "expected an operator after `%s' (= != ~ !~ < <= > >=)",
		     field);
		return NULL;
	}

	n = g_new0(Node, 1);
	n->kind = NODE_COND;
	n->field = g_steal_pointer(&field);
	n->op = op;
	n->value = parse_value(p);
	if (n->value == NULL) {
		node_free(n);
		return NULL;
	}
	if (op == OP_REGEX || op == OP_NOT_REGEX) {
		g_autoptr(GError) rerr = NULL;

		n->regex = g_regex_new(n->value, G_REGEX_DEFAULT,
		                       G_REGEX_MATCH_DEFAULT, &rerr);
		if (n->regex == NULL) {
			fail(p, GOWL_MACRO_FILTER_ERROR_REGEX, "bad regex `%s': %s",
			     n->value, rerr->message);
			node_free(n);
			return NULL;
		}
	}
	return n;
}

static Node *
parse_unary(Parser *p)
{
	Node *n;

	skip_space(p);
	if (eat_keyword(p, "not")
	    || (p->text[p->pos] == '!' && p->text[p->pos + 1] != '='
	        && p->text[p->pos + 1] != '~' && eat(p, "!"))) {
		Node *inner = parse_unary(p);

		if (inner == NULL)
			return NULL;
		n = g_new0(Node, 1);
		n->kind = NODE_NOT;
		n->left = inner;
		return n;
	}
	if (eat(p, "(")) {
		n = parse_or(p);
		if (n == NULL)
			return NULL;
		skip_space(p);
		if (!eat(p, ")")) {
			fail(p, GOWL_MACRO_FILTER_ERROR_SYNTAX, "expected `)'");
			node_free(n);
			return NULL;
		}
		return n;
	}
	return parse_cond(p);
}

static Node *
join(
	NodeKind  kind,
	Node     *left,
	Node     *right
){
	Node *n = g_new0(Node, 1);

	n->kind = kind;
	n->left = left;
	n->right = right;
	return n;
}

static Node *
parse_and(Parser *p)
{
	Node *left = parse_unary(p);

	while (left != NULL) {
		Node *right;

		skip_space(p);
		if (!eat(p, "&&") && !eat_keyword(p, "and"))
			break;
		right = parse_unary(p);
		if (right == NULL) {
			node_free(left);
			return NULL;
		}
		left = join(NODE_AND, left, right);
	}
	return left;
}

static Node *
parse_or(Parser *p)
{
	Node *left = parse_and(p);

	while (left != NULL) {
		Node *right;

		skip_space(p);
		if (!eat(p, "||") && !eat_keyword(p, "or"))
			break;
		right = parse_and(p);
		if (right == NULL) {
			node_free(left);
			return NULL;
		}
		left = join(NODE_OR, left, right);
	}
	return left;
}

/**
 * gowl_macro_filter_parse: (skip)
 * @text: the filter expression (what goes between the brackets)
 * @error: return location for a #GError naming the column
 *
 * Returns: (transfer full) (nullable): the filter, %NULL on an error
 */
GowlMacroFilter *
gowl_macro_filter_parse(
	const gchar  *text,
	GError      **error
){
	GowlMacroFilter *self;
	Parser p;
	Node *root;

	g_return_val_if_fail(text != NULL, NULL);

	p.text = text;
	p.pos = 0;
	p.error = error;
	p.failed = FALSE;
	root = parse_or(&p);
	if (root != NULL) {
		skip_space(&p);
		if (p.text[p.pos] != '\0') {
			fail(&p, GOWL_MACRO_FILTER_ERROR_SYNTAX,
			     "unexpected `%s' (conditions are joined with and/or)",
			     p.text + p.pos);
			node_free(root);
			root = NULL;
		}
	}
	if (root == NULL) {
		if (!p.failed)
			fail(&p, GOWL_MACRO_FILTER_ERROR_SYNTAX, "empty filter");
		return NULL;
	}
	self = g_new0(GowlMacroFilter, 1);
	self->root = root;
	return self;
}

/**
 * gowl_macro_filter_free: (skip)
 * @self: (nullable): a filter
 */
void
gowl_macro_filter_free(GowlMacroFilter *self)
{
	if (self == NULL)
		return;
	node_free(self->root);
	g_free(self);
}

/* Both sides numbers?  Then a numeric comparison; else strcmp. */
static gint
compare(
	const gchar *a,
	const gchar *b
){
	gchar *end_a;
	gchar *end_b;
	gdouble x;
	gdouble y;

	x = g_ascii_strtod(a, &end_a);
	y = g_ascii_strtod(b, &end_b);
	if (*a != '\0' && *b != '\0' && *end_a == '\0' && *end_b == '\0')
		return x < y ? -1 : x > y ? 1 : 0;
	return g_strcmp0(a, b);
}

static gboolean
eval_node(
	const Node *n,
	GHashTable *fields
){
	const gchar *v;

	switch (n->kind) {
	case NODE_AND:
		return eval_node(n->left, fields) && eval_node(n->right, fields);
	case NODE_OR:
		return eval_node(n->left, fields) || eval_node(n->right, fields);
	case NODE_NOT:
		return !eval_node(n->left, fields);
	case NODE_COND:
	default:
		break;
	}
	v = fields != NULL ? g_hash_table_lookup(fields, n->field) : NULL;
	if (v == NULL)
		v = "";
	switch (n->op) {
	case OP_GLOB:      return g_pattern_match_simple(n->value, v);
	case OP_NOT_GLOB:  return !g_pattern_match_simple(n->value, v);
	case OP_REGEX:     return g_regex_match(n->regex, v, 0, NULL);
	case OP_NOT_REGEX: return !g_regex_match(n->regex, v, 0, NULL);
	case OP_LT:        return compare(v, n->value) < 0;
	case OP_LE:        return compare(v, n->value) <= 0;
	case OP_GT:        return compare(v, n->value) > 0;
	case OP_GE:        return compare(v, n->value) >= 0;
	default:           return FALSE;
	}
}

/**
 * gowl_macro_filter_eval: (skip)
 * @self: a filter
 * @fields: (element-type utf8 utf8) (nullable): field -> value
 *
 * Returns: whether the event passes.  `and' and `or' short-circuit.
 */
gboolean
gowl_macro_filter_eval(
	const GowlMacroFilter *self,
	GHashTable            *fields
){
	g_return_val_if_fail(self != NULL, FALSE);
	return eval_node(self->root, fields);
}

static void
append_quoted(
	GString     *out,
	const gchar *v
){
	const gchar *c;

	g_string_append_c(out, '"');
	for (c = v; *c != '\0'; c++) {
		if (*c == '"' || *c == '\\')
			g_string_append_c(out, '\\');
		g_string_append_c(out, *c);
	}
	g_string_append_c(out, '"');
}

static void
node_to_string(
	const Node *n,
	GString    *out
){
	switch (n->kind) {
	case NODE_AND:
	case NODE_OR:
		g_string_append_c(out, '(');
		node_to_string(n->left, out);
		g_string_append(out, n->kind == NODE_AND ? " and " : " or ");
		node_to_string(n->right, out);
		g_string_append_c(out, ')');
		return;
	case NODE_NOT:
		g_string_append(out, "not ");
		node_to_string(n->left, out);
		return;
	case NODE_COND:
	default:
		g_string_append(out, n->field);
		g_string_append(out, op_names[n->op]);
		append_quoted(out, n->value);
		return;
	}
}

/**
 * gowl_macro_filter_to_string: (skip)
 * @self: a filter
 *
 * The filter fully parenthesised and quoted -- how it was understood.
 * It parses back to the same filter.
 *
 * Returns: (transfer full): the text
 */
gchar *
gowl_macro_filter_to_string(const GowlMacroFilter *self)
{
	GString *out;

	g_return_val_if_fail(self != NULL, NULL);
	out = g_string_new(NULL);
	node_to_string(self->root, out);
	return g_string_free(out, FALSE);
}

/**
 * gowl_macro_filter_known_fields: (skip)
 *
 * Returns: (transfer none) (array zero-terminated=1): every field name
 */
const gchar * const *
gowl_macro_filter_known_fields(void)
{
	return known_fields;
}

/**
 * gowl_macro_filter_split_line: (skip)
 * @line: a trigger line, "WHAT [FILTER]: REST"
 * @out_what: (out) (transfer full): "client-added" or "every 1000"
 * @out_filter: (out) (transfer full) (nullable): the filter text, or
 *   %NULL when the line has none
 * @out_rest: (out) (transfer full): "MACRO ARGS"
 * @error: return location for a #GError
 *
 * The `:' that ends WHAT is the first one outside the brackets and
 * outside quotes, so a filter may test for a colon (title=*a:b*).
 *
 * Returns: %TRUE when the line has that shape
 */
gboolean
gowl_macro_filter_split_line(
	const gchar  *line,
	gchar       **out_what,
	gchar       **out_filter,
	gchar       **out_rest,
	GError      **error
){
	const gchar *c;
	const gchar *open = NULL;
	const gchar *close = NULL;
	const gchar *colon = NULL;
	gint depth = 0;
	gchar quote = '\0';

	*out_what = NULL;
	*out_filter = NULL;
	*out_rest = NULL;
	for (c = line; *c != '\0' && colon == NULL; c++) {
		if (quote != '\0') {
			if (*c == '\\' && c[1] != '\0')
				c++;
			else if (*c == quote)
				quote = '\0';
			continue;
		}
		if (depth > 0 && (*c == '"' || *c == '\'')) {
			quote = *c;
		} else if (*c == '[') {
			if (depth == 0 && open != NULL) {
				g_set_error(error, GOWL_MACRO_FILTER_ERROR,
				            GOWL_MACRO_FILTER_ERROR_SYNTAX,
				            "one [filter] per trigger; join conditions "
				            "with and/or");
				return FALSE;
			}
			if (depth == 0)
				open = c;
			depth++;
		} else if (*c == ']') {
			if (depth == 0) {
				g_set_error(error, GOWL_MACRO_FILTER_ERROR,
				            GOWL_MACRO_FILTER_ERROR_SYNTAX,
				            "`]' without `['");
				return FALSE;
			}
			if (--depth == 0)
				close = c;
		} else if (*c == ':' && depth == 0) {
			colon = c;
		}
	}
	if (quote != '\0' || depth > 0) {
		g_set_error(error, GOWL_MACRO_FILTER_ERROR,
		            GOWL_MACRO_FILTER_ERROR_SYNTAX,
		            quote != '\0' ? "unterminated quote in the filter"
		                          : "`[' without `]'");
		return FALSE;
	}
	if (colon == NULL) {
		g_set_error(error, GOWL_MACRO_FILTER_ERROR,
		            GOWL_MACRO_FILTER_ERROR_SYNTAX,
		            "expected \"EVENT [FILTER]: MACRO\"");
		return FALSE;
	}
	if (open != NULL) {
		const gchar *between;

		/* Nothing but space between `]' and `:' */
		for (between = close + 1; between < colon; between++) {
			if (!g_ascii_isspace(*between)) {
				g_set_error(error, GOWL_MACRO_FILTER_ERROR,
				            GOWL_MACRO_FILTER_ERROR_SYNTAX,
				            "the [filter] goes right before the `:'");
				return FALSE;
			}
		}
		*out_what = g_strstrip(g_strndup(line, (gsize)(open - line)));
		*out_filter = g_strstrip(g_strndup(open + 1,
		                                   (gsize)(close - open - 1)));
	} else {
		*out_what = g_strstrip(g_strndup(line, (gsize)(colon - line)));
	}
	*out_rest = g_strstrip(g_strdup(colon + 1));
	return TRUE;
}
