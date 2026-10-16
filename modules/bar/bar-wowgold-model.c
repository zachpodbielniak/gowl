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
 * The `wow-gold' widget's model: tsmctl JSON in, money and argv out.
 *
 * Why a JSON walker of its own rather than barkit's gowl-bar-json.h:
 * tsmctl's documents are three arrays deep (sections -> tables ->
 * rows) and every amount in them is a NUMBER.  The kit reads strings
 * and booleans by anchoring on the first `"key":' anywhere below the
 * point it is given, and has no number reader at all; bar.so does not
 * link json-glib.  So this file walks only the TOP-LEVEL members of
 * one object at a time, skipping strings and nested values wholesale,
 * which is the property that keeps a field missing from one row from
 * being answered out of the next one.  It is still not a validating
 * parser: what it guarantees is that it fails with %NULL / %FALSE
 * rather than with a plausible wrong answer.
 */

#include "bar-wowgold-model.h"

#include <string.h>
#include <errno.h>

/* ----------------------------------------------------------------
 * Money
 * ---------------------------------------------------------------- */

/**
 * bar_wowgold_money_format_parse:
 * @text: (nullable): the `money-format' setting
 *
 * "gsc" (any case) is gold-silver-copper; anything else, including
 * unset, is whole gold -- the setting's documented default.
 *
 * Returns: the format
 */
BarWowgoldMoneyFormat
bar_wowgold_money_format_parse(const gchar *text)
{
	if (text != NULL && g_ascii_strcasecmp(text, "gsc") == 0)
		return BAR_WOWGOLD_MONEY_GSC;
	return BAR_WOWGOLD_MONEY_G;
}

/*
 * The magnitude of @copper as unsigned, without the undefined
 * behaviour of negating G_MININT64.
 */
static guint64
money_magnitude(gint64 copper)
{
	if (copper >= 0)
		return (guint64)copper;
	return (guint64)(-(copper + 1)) + 1u;
}

/* Appends @value with a comma every three digits.  Always a comma:
   it is how the game and tsmctl both write gold, and a bar label that
   changed with LC_NUMERIC would disagree with the game it describes. */
static void
append_grouped(GString *out, guint64 value)
{
	gchar digits[32];
	gsize len, i;

	g_snprintf(digits, sizeof(digits), "%" G_GUINT64_FORMAT, value);
	len = strlen(digits);
	for (i = 0; i < len; i++) {
		if (i > 0 && (len - i) % 3 == 0)
			g_string_append_c(out, ',');
		g_string_append_c(out, digits[i]);
	}
}

/**
 * bar_wowgold_format_money:
 * @copper: an amount in copper
 * @format: whole gold, or gold-silver-copper
 *
 * Whole gold truncates the silver and copper, as the game's own gold
 * display does.  An amount under one gold is written in silver and
 * copper even in the whole-gold format, so a 50-silver postage charge
 * reads "50s" and not a misleading "0g".
 *
 * Returns: (transfer full): e.g. "123,456g", "-12g", "1,234g 5s 7c"
 */
gchar *
bar_wowgold_format_money(gint64 copper, BarWowgoldMoneyFormat format)
{
	GString *out;
	guint64 mag, gold, silver, cop;
	gboolean any;

	mag    = money_magnitude(copper);
	gold   = mag / BAR_WOWGOLD_COPPER_PER_GOLD;
	silver = (mag / BAR_WOWGOLD_COPPER_PER_SILVER) % 100u;
	cop    = mag % 100u;

	out = g_string_new(NULL);
	if (copper < 0)
		g_string_append_c(out, '-');

	if (format == BAR_WOWGOLD_MONEY_G && (gold > 0 || mag == 0)) {
		append_grouped(out, gold);
		g_string_append_c(out, 'g');
		return g_string_free(out, FALSE);
	}

	/* Gold-silver-copper: the units that are there, and copper
	   alone for zero. */
	any = FALSE;
	if (gold > 0) {
		append_grouped(out, gold);
		g_string_append_c(out, 'g');
		any = TRUE;
	}
	if (silver > 0) {
		if (any)
			g_string_append_c(out, ' ');
		g_string_append_printf(out, "%" G_GUINT64_FORMAT "s", silver);
		any = TRUE;
	}
	if (cop > 0 || !any) {
		if (any)
			g_string_append_c(out, ' ');
		g_string_append_printf(out, "%" G_GUINT64_FORMAT "c", cop);
	}
	return g_string_free(out, FALSE);
}

/**
 * bar_wowgold_format_compact:
 * @copper: an amount in copper
 *
 * Three significant figures and a K/M/B/T suffix, for the bar: a
 * nine-digit total does not belong in a status line.  Amounts under
 * ten thousand gold are short enough already and are written in full.
 * Rounding that reaches 1000 of a unit moves up a unit, so 999,999g is
 * "1.00Mg" rather than "1000Kg".  The decimal point is always `.'
 * (g_ascii_formatd), whatever the locale.
 *
 * Returns: (transfer full): e.g. "35.6Mg", "-1.20Kg", "9,999g"
 */
gchar *
bar_wowgold_format_compact(gint64 copper)
{
	static const gchar units[] = { 'K', 'M', 'B', 'T' };
	gdouble divisor, v;
	guint64 gold;
	guint u;
	gchar buf[G_ASCII_DTOSTR_BUF_SIZE];
	const gchar *fmt;

	gold = money_magnitude(copper) / BAR_WOWGOLD_COPPER_PER_GOLD;
	if (gold < 10000u)
		return bar_wowgold_format_money(copper, BAR_WOWGOLD_MONEY_G);

	/* Start at the largest unit that fits, then let rounding push
	   it up one if it has to. */
	u = 0;
	divisor = 1e3;
	while (u + 1 < G_N_ELEMENTS(units) && (gdouble)gold >= divisor * 1e3) {
		divisor *= 1e3;
		u++;
	}
	for (;;) {
		v = (gdouble)gold / divisor;
		fmt = (v < 10.0) ? "%.2f" : (v < 100.0) ? "%.1f" : "%.0f";
		g_ascii_formatd(buf, sizeof(buf), fmt, v);
		if (g_ascii_strtod(buf, NULL) < 1000.0 ||
		    u + 1 >= G_N_ELEMENTS(units))
			break;
		divisor *= 1e3;
		u++;
	}
	return g_strdup_printf("%s%s%cg", (copper < 0) ? "-" : "", buf,
	                       units[u]);
}

/**
 * bar_wowgold_format_delta:
 * @copper: a change, in copper
 * @format: the money format
 * @compact: use the bar's compact form
 *
 * A signed amount: a gain gets an explicit `+', so "+150,000g" and
 * "-80,000g" line up as changes rather than reading as balances.
 *
 * Returns: (transfer full): the text
 */
gchar *
bar_wowgold_format_delta(gint64 copper, BarWowgoldMoneyFormat format,
                         gboolean compact)
{
	g_autofree gchar *body = NULL;

	body = compact ? bar_wowgold_format_compact(copper)
	               : bar_wowgold_format_money(copper, format);
	return g_strdup_printf("%s%s", (copper > 0) ? "+" : "", body);
}

/**
 * bar_wowgold_parse_money:
 * @text: an amount: "123g 45s 6c", "1,234g", "1.5g", "-5g", "12s",
 *   or a bare integer, which is copper (tsmctl's machine form)
 * @copper: (out): the amount in copper
 *
 * Used where tsmctl hands back a string for a money field --- its JSON
 * passes a pre-formatted string through untouched --- so a newer
 * tsmctl that formats a value cannot silently zero it.  A fraction is
 * accepted on gold only (to four places, which is copper).  Overflow,
 * an unknown unit, a repeated sign or trailing junk all fail rather
 * than returning a part of the number.
 *
 * Returns: %TRUE when the whole of @text was an amount
 */
gboolean
bar_wowgold_parse_money(const gchar *text, gint64 *copper)
{
	const gchar *p;
	gboolean neg = FALSE, any = FALSE;
	guint64 total = 0;

	g_return_val_if_fail(copper != NULL, FALSE);
	if (text == NULL)
		return FALSE;

	p = text;
	while (g_ascii_isspace(*p))
		p++;
	if (*p == '-' || *p == '+') {
		neg = (*p == '-');
		p++;
	}

	for (;;) {
		guint64 whole = 0, frac = 0, denom = 1, mult, value;
		gboolean digits = FALSE;

		while (g_ascii_isspace(*p))
			p++;
		if (*p == '\0')
			break;

		/* The integer part, commas allowed between digits. */
		while (g_ascii_isdigit(*p) || (*p == ',' && digits &&
		                              g_ascii_isdigit(p[1]))) {
			if (*p != ',') {
				if (whole > (G_MAXUINT64 - 9u) / 10u)
					return FALSE;
				whole = whole * 10u + (guint64)(*p - '0');
				digits = TRUE;
			}
			p++;
		}
		if (*p == '.' && g_ascii_isdigit(p[1])) {
			p++;
			while (g_ascii_isdigit(*p)) {
				if (denom < 10000u) {
					frac = frac * 10u + (guint64)(*p - '0');
					denom *= 10u;
				}
				p++;
			}
		}
		if (!digits && denom == 1)
			return FALSE;

		while (g_ascii_isspace(*p))
			p++;
		switch (g_ascii_tolower(*p)) {
		case 'g':
			mult = BAR_WOWGOLD_COPPER_PER_GOLD;
			p++;
			break;
		case 's':
			mult = BAR_WOWGOLD_COPPER_PER_SILVER;
			p++;
			break;
		case 'c':
			mult = 1;
			p++;
			break;
		case '\0':
			/* A bare number is copper, and only on its own. */
			if (any)
				return FALSE;
			mult = 1;
			break;
		default:
			return FALSE;
		}
		/* A fraction of a silver or a copper is not money. */
		if (denom > 1 && mult != BAR_WOWGOLD_COPPER_PER_GOLD)
			return FALSE;

		if (whole > G_MAXUINT64 / mult)
			return FALSE;
		value = whole * mult + (frac * mult) / denom;
		if (value > (guint64)G_MAXINT64 - total)
			return FALSE;
		total += value;
		any = TRUE;
	}

	if (!any)
		return FALSE;
	*copper = neg ? -(gint64)total : (gint64)total;
	return TRUE;
}

/* ----------------------------------------------------------------
 * JSON
 * ---------------------------------------------------------------- */

static const gchar *
json_ws(const gchar *p)
{
	while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
		p++;
	return p;
}

/* @p is on an opening quote.  Returns the byte after the closing one,
   or NULL for an unterminated string. */
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

/*
 * The end of the value starting at @p.  Containers are matched by
 * depth with strings skipped whole, so a brace inside an item name
 * cannot unbalance the walk.  Iterative, so a hostile document of a
 * million `[' costs a loop, not the stack.
 */
static const gchar *
json_skip_value(const gchar *p)
{
	const gchar *start;

	p = json_ws(p);
	if (*p == '"')
		return json_skip_string(p);
	if (*p == '{' || *p == '[') {
		glong depth = 0;

		while (*p != '\0') {
			if (*p == '"') {
				p = json_skip_string(p);
				if (p == NULL)
					return NULL;
				continue;
			}
			if (*p == '{' || *p == '[') {
				depth++;
			} else if (*p == '}' || *p == ']') {
				depth--;
				if (depth == 0)
					return p + 1;
				if (depth < 0)
					return NULL;
			}
			p++;
		}
		return NULL;
	}
	/* A scalar: number, true, false, null. */
	start = p;
	while (*p != '\0' && *p != ',' && *p != '}' && *p != ']' &&
	       *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r')
		p++;
	return (p == start) ? NULL : p;
}

static gint
hex_value(gchar c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

/* Four hex digits at @p, or -1. */
static gint
hex4(const gchar *p)
{
	gint i, v = 0, d;

	for (i = 0; i < 4; i++) {
		d = hex_value(p[i]);
		if (d < 0)
			return -1;
		v = v * 16 + d;
	}
	return v;
}

/*
 * Unescape the string token [@start, @end) (quotes included).  \u
 * escapes become UTF-8, surrogate pairs joined; a lone surrogate or
 * \u0000 becomes U+FFFD, since a NUL in the middle would truncate the
 * name silently further on.  The result may still be invalid UTF-8 if
 * the raw bytes were; callers that show it sanitise it.
 */
static gchar *
json_unescape(const gchar *start, const gchar *end)
{
	GString *out;
	const gchar *p;

	if (end - start < 2 || *start != '"' || end[-1] != '"')
		return NULL;

	out = g_string_sized_new((gsize)(end - start));
	p = start + 1;
	while (p < end - 1) {
		gunichar ch;
		gint hi, lo;

		if (*p != '\\') {
			g_string_append_c(out, *p);
			p++;
			continue;
		}
		p++;
		switch (*p) {
		case 'n': g_string_append_c(out, '\n'); p++; break;
		case 't': g_string_append_c(out, '\t'); p++; break;
		case 'r': g_string_append_c(out, '\r'); p++; break;
		case 'b': g_string_append_c(out, '\b'); p++; break;
		case 'f': g_string_append_c(out, '\f'); p++; break;
		case 'u':
			hi = (end - 1 - (p + 1) >= 4) ? hex4(p + 1) : -1;
			if (hi < 0) {
				g_string_append_unichar(out, 0xFFFDu);
				p++;
				break;
			}
			p += 5;
			ch = (gunichar)hi;
			if (hi >= 0xD800 && hi <= 0xDBFF) {
				lo = (end - 1 - p >= 6 && p[0] == '\\' &&
				      p[1] == 'u') ? hex4(p + 2) : -1;
				if (lo >= 0xDC00 && lo <= 0xDFFF) {
					ch = 0x10000u +
					     (((gunichar)hi - 0xD800u) << 10) +
					     ((gunichar)lo - 0xDC00u);
					p += 6;
				} else {
					ch = 0xFFFDu;
				}
			} else if ((hi >= 0xDC00 && hi <= 0xDFFF) || hi == 0) {
				ch = 0xFFFDu;
			}
			g_string_append_unichar(out, ch);
			break;
		default:
			/* \" \\ \/ and anything unknown: the character
			   itself. */
			if (p < end - 1) {
				g_string_append_c(out, *p);
				p++;
			}
			break;
		}
	}
	return g_string_free(out, FALSE);
}

/*
 * Make text from tsmctl fit to draw: valid UTF-8, no control
 * characters (a newline in an item name would split a panel row), and
 * at most BAR_WOWGOLD_TEXT_MAX characters.
 */
static gchar *
sanitise_text(const gchar *raw)
{
	g_autofree gchar *valid = NULL;
	GString *out;
	const gchar *p;
	glong n = 0;

	valid = g_utf8_make_valid(raw, -1);
	out = g_string_new(NULL);
	for (p = valid; *p != '\0'; p = g_utf8_next_char(p)) {
		gunichar ch = g_utf8_get_char(p);

		if (n >= BAR_WOWGOLD_TEXT_MAX) {
			g_string_append(out, "\xe2\x80\xa6");
			break;
		}
		if (ch < 0x20u || ch == 0x7Fu)
			g_string_append_c(out, ' ');
		else
			g_string_append_unichar(out, ch);
		n++;
	}
	return g_string_free(out, FALSE);
}

/**
 * bar_wowgold_json_decode:
 * @token: a raw JSON string token, quotes included
 *
 * Returns: (transfer full) (nullable): the string, unescaped and made
 *   safe to draw (see sanitise_text), or %NULL when @token is not a
 *   string
 */
gchar *
bar_wowgold_json_decode(const gchar *token)
{
	g_autofree gchar *raw = NULL;

	if (token == NULL || token[0] != '"')
		return NULL;
	raw = json_unescape(token, token + strlen(token));
	if (raw == NULL)
		return NULL;
	return sanitise_text(raw);
}

/**
 * bar_wowgold_json_member:
 * @object: (nullable): a JSON object's text
 * @key: a member name
 *
 * The raw text of @object's own member @key --- never a member of an
 * object nested inside it, and never a match inside a string.
 *
 * Returns: (transfer full) (nullable): the value's text, or %NULL
 */
gchar *
bar_wowgold_json_member(const gchar *object, const gchar *key)
{
	const gchar *p;

	if (object == NULL || key == NULL)
		return NULL;
	p = json_ws(object);
	if (*p != '{')
		return NULL;
	p++;

	for (;;) {
		const gchar *kend, *vend;
		g_autofree gchar *name = NULL;

		p = json_ws(p);
		if (*p != '"')
			return NULL;   /* `}' (no such member) or garbage */
		kend = json_skip_string(p);
		if (kend == NULL)
			return NULL;
		name = json_unescape(p, kend);

		p = json_ws(kend);
		if (*p != ':')
			return NULL;
		p = json_ws(p + 1);
		vend = json_skip_value(p);
		if (vend == NULL)
			return NULL;
		if (g_strcmp0(name, key) == 0)
			return g_strndup(p, (gsize)(vend - p));

		p = json_ws(vend);
		if (*p != ',')
			return NULL;
		p++;
	}
}

/**
 * bar_wowgold_json_int:
 * @object: (nullable): a JSON object's text
 * @key: a member name
 * @out: (out): the value
 *
 * Reads a whole number.  A fractional number is rounded; a string is
 * read as money (bar_wowgold_parse_money()), since tsmctl passes a
 * pre-formatted value through as one.  `null', a missing member and a
 * number past 64 bits all fail --- a gold total that overflowed would
 * otherwise come back as a confident, wrong, negative number.
 *
 * Returns: %TRUE when @out was set
 */
gboolean
bar_wowgold_json_int(const gchar *object, const gchar *key, gint64 *out)
{
	g_autofree gchar *raw = NULL;
	gchar *end = NULL;
	gint64 v;
	gdouble d;

	g_return_val_if_fail(out != NULL, FALSE);
	raw = bar_wowgold_json_member(object, key);
	if (raw == NULL || g_strcmp0(raw, "null") == 0)
		return FALSE;

	if (raw[0] == '"') {
		g_autofree gchar *text = bar_wowgold_json_decode(raw);

		return bar_wowgold_parse_money(text, out);
	}

	errno = 0;
	v = g_ascii_strtoll(raw, &end, 10);
	if (end != raw && *end == '\0' && errno == 0) {
		*out = v;
		return TRUE;
	}
	if (errno == ERANGE)
		return FALSE;

	/* 12.5, 1e3: a float.  Bounded well inside gint64 before the
	   cast, which is otherwise undefined. */
	errno = 0;
	d = g_ascii_strtod(raw, &end);
	if (end == raw || *end != '\0' || errno != 0 ||
	    d != d || d > 9.0e18 || d < -9.0e18)
		return FALSE;
	*out = (gint64)((d < 0) ? d - 0.5 : d + 0.5);
	return TRUE;
}

/**
 * bar_wowgold_json_text:
 * @object: (nullable): a JSON object's text
 * @key: a member name
 *
 * Returns: (transfer full) (nullable): the member's string value, made
 *   safe to draw, or %NULL when it is absent or not a string
 */
gchar *
bar_wowgold_json_text(const gchar *object, const gchar *key)
{
	g_autofree gchar *raw = NULL;

	raw = bar_wowgold_json_member(object, key);
	return bar_wowgold_json_decode(raw);
}

/**
 * bar_wowgold_json_foreach:
 * @array: (nullable): a JSON array's text
 * @func: (scope call): called once per element
 * @user_data: passed to @func
 *
 * Walks at most %BAR_WOWGOLD_ROWS_MAX elements.  Stops quietly at the
 * first element it cannot delimit, keeping what came before it.
 *
 * Returns: how many elements @func saw
 */
guint
bar_wowgold_json_foreach(const gchar *array, BarWowgoldJsonFunc func,
                         gpointer user_data)
{
	const gchar *p;
	guint index = 0;

	if (array == NULL)
		return 0;
	p = json_ws(array);
	if (*p != '[')
		return 0;
	p++;

	while (index < BAR_WOWGOLD_ROWS_MAX) {
		const gchar *vend;
		g_autofree gchar *element = NULL;
		gboolean more;

		p = json_ws(p);
		if (*p == ']' || *p == '\0')
			break;
		vend = json_skip_value(p);
		if (vend == NULL)
			break;
		element = g_strndup(p, (gsize)(vend - p));
		more = func(element, index, user_data);
		index++;
		if (!more)
			break;
		p = json_ws(vend);
		if (*p != ',')
			break;
		p++;
	}
	return index;
}

/* ----------------------------------------------------------------
 * Documents
 * ---------------------------------------------------------------- */

static void
char_free(gpointer p)
{
	BarWowgoldChar *c = p;

	g_free(c->name);
	g_free(c->realm);
	g_free(c->kind);
	g_free(c);
}

static void
sale_free(gpointer p)
{
	BarWowgoldSale *s = p;

	g_free(s->item);
	g_free(s->character);
	g_free(s->realm);
	g_free(s->source);
	g_free(s);
}

static void
attention_free(gpointer p)
{
	BarWowgoldAttention *a = p;

	g_free(a->what);
	g_free(a->character);
	g_free(a->realm);
	g_free(a);
}

/**
 * bar_wowgold_data_new:
 *
 * Returns: (transfer full): an empty reading
 */
BarWowgoldData *
bar_wowgold_data_new(void)
{
	BarWowgoldData *d;

	d = g_new0(BarWowgoldData, 1);
	d->chars     = g_ptr_array_new_with_free_func(char_free);
	d->points    = g_array_new(FALSE, TRUE, sizeof(BarWowgoldPoint));
	d->sales     = g_ptr_array_new_with_free_func(sale_free);
	d->attention = g_ptr_array_new_with_free_func(attention_free);
	return d;
}

/**
 * bar_wowgold_data_free:
 * @data: (nullable) (transfer full): a reading
 */
void
bar_wowgold_data_free(BarWowgoldData *data)
{
	if (data == NULL)
		return;
	g_ptr_array_unref(data->chars);
	g_array_unref(data->points);
	g_ptr_array_unref(data->sales);
	g_ptr_array_unref(data->attention);
	g_free(data);
}

/*
 * Every table in a tsmctl document, in order.  @func gets each table
 * object's text.  Returns FALSE when the text is not a tsmctl document
 * at all (no `sections' array) --- which is how an ssh banner, a Python
 * traceback on stdout or an old tsmctl's text output is told apart
 * from a document that is merely empty.
 */
typedef void (*TableFunc) (const gchar *table, gpointer user_data);

typedef struct {
	TableFunc func;
	gpointer  user_data;
} TableWalk;

static gboolean
walk_table_cb(const gchar *table, guint index, gpointer user_data)
{
	TableWalk *w = user_data;

	(void)index;
	w->func(table, w->user_data);
	return TRUE;
}

static gboolean
walk_section_cb(const gchar *section, guint index, gpointer user_data)
{
	g_autofree gchar *tables = NULL;

	(void)index;
	tables = bar_wowgold_json_member(section, "tables");
	bar_wowgold_json_foreach(tables, walk_table_cb, user_data);
	return TRUE;
}

static gboolean
walk_tables(const gchar *json, TableFunc func, gpointer user_data)
{
	g_autofree gchar *sections = NULL;
	TableWalk w;
	const gchar *p;

	sections = bar_wowgold_json_member(json, "sections");
	if (sections == NULL)
		return FALSE;
	p = json_ws(sections);
	if (*p != '[')
		return FALSE;
	w.func = func;
	w.user_data = user_data;
	bar_wowgold_json_foreach(sections, walk_section_cb, &w);
	return TRUE;
}

/* A string member, or "" when absent -- for the optional columns. */
static gchar *
text_or_empty(const gchar *object, const gchar *key)
{
	gchar *t = bar_wowgold_json_text(object, key);

	return (t != NULL) ? t : g_strdup("");
}

/* Saturating add: a hostile document of huge balances pins the total
   at the limit rather than wrapping it negative. */
static gint64
add_sat(gint64 a, gint64 b)
{
	if (b > 0 && a > G_MAXINT64 - b)
		return G_MAXINT64;
	if (b < 0 && a < G_MININT64 - b)
		return G_MININT64;
	return a + b;
}

/* --- get gold --- */

typedef struct {
	BarWowgoldData *data;
	gboolean        have_totals;
	gint64          totals;
	gint64          sum;
} GoldWalk;

static gboolean
gold_row_cb(const gchar *row, guint index, gpointer user_data)
{
	GoldWalk *w = user_data;
	BarWowgoldChar *c;
	gint64 copper;

	(void)index;
	if (!bar_wowgold_json_int(row, "gold_copper", &copper))
		return TRUE;
	c = g_new0(BarWowgoldChar, 1);
	c->name = bar_wowgold_json_text(row, "owner");
	if (c->name == NULL || c->name[0] == '\0') {
		g_free(c->name);
		c->name = g_strdup("?");
	}
	c->realm  = text_or_empty(row, "realm");
	c->kind   = text_or_empty(row, "kind");
	c->copper = copper;
	w->sum = add_sat(w->sum, copper);
	g_ptr_array_add(w->data->chars, c);
	return TRUE;
}

static void
gold_table_cb(const gchar *table, gpointer user_data)
{
	GoldWalk *w = user_data;
	g_autofree gchar *rows = NULL;
	g_autofree gchar *totals = NULL;
	gint64 v;

	rows = bar_wowgold_json_member(table, "rows");
	bar_wowgold_json_foreach(rows, gold_row_cb, w);
	totals = bar_wowgold_json_member(table, "totals");
	if (bar_wowgold_json_int(totals, "gold_copper", &v)) {
		w->have_totals = TRUE;
		w->totals = v;
	}
}

static gint
char_cmp(gconstpointer a, gconstpointer b)
{
	const BarWowgoldChar *x = *(BarWowgoldChar * const *)a;
	const BarWowgoldChar *y = *(BarWowgoldChar * const *)b;

	if (x->copper != y->copper)
		return (x->copper > y->copper) ? -1 : 1;
	return g_strcmp0(x->name, y->name);
}

/**
 * bar_wowgold_parse_gold:
 * @data: the reading to fill
 * @json: `tsmctl -o json get gold'
 *
 * Fills @data's characters, richest first, and its total.  The total
 * is tsmctl's own TOTAL row when there is one (it counts warbanks and
 * guild banks the same way the report does) and the sum of the rows
 * otherwise.
 *
 * Returns: %FALSE when @json is not a tsmctl document
 */
gboolean
bar_wowgold_parse_gold(BarWowgoldData *data, const gchar *json)
{
	GoldWalk w;

	g_return_val_if_fail(data != NULL, FALSE);
	memset(&w, 0, sizeof(w));
	w.data = data;
	g_ptr_array_set_size(data->chars, 0);
	if (!walk_tables(json, gold_table_cb, &w))
		return FALSE;
	g_ptr_array_sort(data->chars, char_cmp);
	data->total = w.have_totals ? w.totals : w.sum;
	data->have_total = TRUE;
	return TRUE;
}

/* --- get gold-history --- */

typedef struct {
	guint32 day;
	gint64  copper;
	guint   order;
} HistRow;

static gboolean
history_row_cb(const gchar *row, guint index, gpointer user_data)
{
	GArray *rows = user_data;
	g_autofree gchar *date = NULL;
	HistRow r;

	(void)index;
	date = bar_wowgold_json_text(row, "date");
	r.day = bar_wowgold_julian(date);
	if (r.day == 0 || !bar_wowgold_json_int(row, "gold_copper", &r.copper))
		return TRUE;
	r.order = rows->len;
	g_array_append_val(rows, r);
	return TRUE;
}

static void
history_table_cb(const gchar *table, gpointer user_data)
{
	g_autofree gchar *rows = NULL;

	rows = bar_wowgold_json_member(table, "rows");
	bar_wowgold_json_foreach(rows, history_row_cb, user_data);
}

static gint
hist_cmp(gconstpointer a, gconstpointer b)
{
	const HistRow *x = a;
	const HistRow *y = b;

	if (x->day != y->day)
		return (x->day < y->day) ? -1 : 1;
	return (x->order < y->order) ? -1 : (x->order > y->order);
}

/**
 * bar_wowgold_parse_history:
 * @data: the reading to fill
 * @json: `tsmctl -o json get gold-history --by=day'
 *
 * One point per local calendar day, oldest first.  tsmctl's last row
 * is "now" rather than end-of-day, so it can share a date with the
 * row before it; the later row wins, as it would for any duplicate.
 * A row with an unreadable date or amount is dropped, not zeroed.
 *
 * Returns: %FALSE when @json is not a tsmctl document
 */
gboolean
bar_wowgold_parse_history(BarWowgoldData *data, const gchar *json)
{
	g_autoptr(GArray) rows = NULL;
	guint i;

	g_return_val_if_fail(data != NULL, FALSE);
	rows = g_array_new(FALSE, TRUE, sizeof(HistRow));
	g_array_set_size(data->points, 0);
	if (!walk_tables(json, history_table_cb, rows))
		return FALSE;

	g_array_sort(rows, hist_cmp);
	for (i = 0; i < rows->len; i++) {
		const HistRow *r = &g_array_index(rows, HistRow, i);
		BarWowgoldPoint pt;

		if (data->points->len > 0) {
			BarWowgoldPoint *last = &g_array_index(data->points,
				BarWowgoldPoint, data->points->len - 1);

			if (last->day == r->day) {
				last->copper = r->copper;
				continue;
			}
		}
		pt.day = r->day;
		pt.copper = r->copper;
		g_array_append_val(data->points, pt);
	}
	return TRUE;
}

/* --- get sales --- */

typedef struct {
	BarWowgoldData *data;
	guint           order;
} SalesWalk;

static gboolean
sales_row_cb(const gchar *row, guint index, gpointer user_data)
{
	SalesWalk *w = user_data;
	BarWowgoldSale *s;
	g_autofree gchar *when = NULL;

	(void)index;
	s = g_new0(BarWowgoldSale, 1);
	s->item = bar_wowgold_json_text(row, "item_name");
	if (s->item == NULL || s->item[0] == '\0') {
		g_free(s->item);
		s->item = bar_wowgold_json_text(row, "item");
	}
	if (s->item == NULL || s->item[0] == '\0') {
		g_free(s->item);
		s->item = g_strdup("(unknown item)");
	}
	s->character = text_or_empty(row, "player");
	s->realm     = text_or_empty(row, "realm");
	s->source    = text_or_empty(row, "source");
	if (!bar_wowgold_json_int(row, "qty", &s->qty))
		s->qty = 1;
	if (!bar_wowgold_json_int(row, "price_copper", &s->price))
		s->price = 0;
	if (!bar_wowgold_json_int(row, "total_copper", &s->total))
		s->total = s->price * MAX(s->qty, 1);

	when = bar_wowgold_json_text(row, "time");
	if (when != NULL) {
		g_autoptr(GDateTime) dt = NULL;

		dt = g_date_time_new_from_iso8601(when, NULL);
		if (dt != NULL)
			s->when = g_date_time_to_unix(dt);
	}
	g_ptr_array_add(w->data->sales, s);
	return TRUE;
}

static void
sales_table_cb(const gchar *table, gpointer user_data)
{
	SalesWalk *w = user_data;
	g_autofree gchar *rows = NULL;
	g_autofree gchar *totals = NULL;
	g_autofree gchar *count = NULL;

	rows = bar_wowgold_json_member(table, "rows");
	bar_wowgold_json_foreach(rows, sales_row_cb, w);

	/* "1867 rows": how many there are, of which we asked for a few. */
	totals = bar_wowgold_json_member(table, "totals");
	count = bar_wowgold_json_text(totals, "time");
	if (count != NULL)
		w->data->sales_listed = g_ascii_strtoll(count, NULL, 10);
}

static gint
sale_cmp(gconstpointer a, gconstpointer b)
{
	const BarWowgoldSale *x = *(BarWowgoldSale * const *)a;
	const BarWowgoldSale *y = *(BarWowgoldSale * const *)b;

	if (x->when != y->when)
		return (x->when > y->when) ? -1 : 1;
	return 0;
}

/**
 * bar_wowgold_parse_sales:
 * @data: the reading to fill
 * @json: `tsmctl -o json get sales'
 *
 * Newest first, by the parsed time rather than by the text: tsmctl
 * writes local times with their offset, and two sides of a daylight
 * saving change do not sort as strings.  (g_ptr_array_sort is a
 * stable merge sort, so equal times keep tsmctl's order.)
 *
 * Returns: %FALSE when @json is not a tsmctl document
 */
gboolean
bar_wowgold_parse_sales(BarWowgoldData *data, const gchar *json)
{
	SalesWalk w;

	g_return_val_if_fail(data != NULL, FALSE);
	w.data = data;
	w.order = 0;
	g_ptr_array_set_size(data->sales, 0);
	data->sales_listed = 0;
	if (!walk_tables(json, sales_table_cb, &w))
		return FALSE;
	g_ptr_array_sort(data->sales, sale_cmp);
	if (data->sales_listed < (gint64)data->sales->len)
		data->sales_listed = (gint64)data->sales->len;
	return TRUE;
}

/* --- pnl --- */

typedef struct {
	BarWowgoldData *data;
	gboolean        found;
} PnlWalk;

static gboolean
pnl_section_cb(const gchar *section, guint index, gpointer user_data)
{
	PnlWalk *w = user_data;
	g_autofree gchar *values = NULL;
	BarWowgoldPnl pnl;

	(void)index;
	values = bar_wowgold_json_member(section, "values");
	if (!bar_wowgold_json_int(values, "Revenue", &pnl.revenue))
		return TRUE;
	/* Each is optional on its own: an older tsmctl without one line
	   still gives the rest. */
	if (!bar_wowgold_json_int(values, "Purchases", &pnl.purchases))
		pnl.purchases = 0;
	if (!bar_wowgold_json_int(values, "Expenses", &pnl.expenses))
		pnl.expenses = 0;
	if (!bar_wowgold_json_int(values, "Net cash flow", &pnl.net))
		pnl.net = pnl.revenue - pnl.purchases - pnl.expenses;
	if (!bar_wowgold_json_int(values, "Units sold", &pnl.units))
		pnl.units = 0;
	w->data->pnl = pnl;
	w->found = TRUE;
	return FALSE;
}

/**
 * bar_wowgold_parse_pnl:
 * @data: the reading to fill
 * @json: `tsmctl -o json pnl --period=today'
 *
 * Reads the first section whose key/value block has a "Revenue" line;
 * those labels are tsmctl's own and are what its text output shows.
 *
 * Returns: %FALSE when @json is not a tsmctl document
 */
gboolean
bar_wowgold_parse_pnl(BarWowgoldData *data, const gchar *json)
{
	g_autofree gchar *sections = NULL;
	PnlWalk w;

	g_return_val_if_fail(data != NULL, FALSE);
	data->have_pnl = FALSE;
	sections = bar_wowgold_json_member(json, "sections");
	if (sections == NULL || *json_ws(sections) != '[')
		return FALSE;
	w.data = data;
	w.found = FALSE;
	bar_wowgold_json_foreach(sections, pnl_section_cb, &w);
	data->have_pnl = w.found;
	return TRUE;
}

/* --- attention --- */

static gboolean
attention_row_cb(const gchar *row, guint index, gpointer user_data)
{
	BarWowgoldData *data = user_data;
	BarWowgoldAttention *a;

	(void)index;
	a = g_new0(BarWowgoldAttention, 1);
	a->what = bar_wowgold_json_text(row, "what");
	if (a->what == NULL || a->what[0] == '\0') {
		g_free(a->what);
		a->what = g_strdup("needs a login");
	}
	a->character = text_or_empty(row, "char");
	a->realm     = text_or_empty(row, "realm");
	if (!bar_wowgold_json_int(row, "count", &a->count))
		a->count = 0;
	if (!bar_wowgold_json_int(row, "value_copper", &a->value))
		a->value = 0;
	g_ptr_array_add(data->attention, a);
	return TRUE;
}

static void
attention_table_cb(const gchar *table, gpointer user_data)
{
	g_autofree gchar *rows = NULL;

	rows = bar_wowgold_json_member(table, "rows");
	bar_wowgold_json_foreach(rows, attention_row_cb, user_data);
}

/**
 * bar_wowgold_parse_attention:
 * @data: the reading to fill
 * @json: `tsmctl -o json attention'
 *
 * In tsmctl's order, which is most urgent first.
 *
 * Returns: %FALSE when @json is not a tsmctl document
 */
gboolean
bar_wowgold_parse_attention(BarWowgoldData *data, const gchar *json)
{
	g_return_val_if_fail(data != NULL, FALSE);
	g_ptr_array_set_size(data->attention, 0);
	data->have_attention = FALSE;
	if (!walk_tables(json, attention_table_cb, data))
		return FALSE;
	data->have_attention = TRUE;
	return TRUE;
}

/* ----------------------------------------------------------------
 * Days and trends
 * ---------------------------------------------------------------- */

/**
 * bar_wowgold_julian:
 * @iso_date: (nullable): text starting "YYYY-MM-DD" (the rest, a time
 *   and an offset, is ignored: tsmctl writes LOCAL times, so the date
 *   part already is the local calendar day)
 *
 * Returns: the Julian day number, or 0 when there is no valid date
 */
guint32
bar_wowgold_julian(const gchar *iso_date)
{
	gint y, m, d, i;
	GDate date;

	if (iso_date == NULL || strlen(iso_date) < 10)
		return 0;
	for (i = 0; i < 10; i++) {
		if (i == 4 || i == 7) {
			if (iso_date[i] != '-')
				return 0;
		} else if (!g_ascii_isdigit(iso_date[i])) {
			return 0;
		}
	}
	y = (gint)g_ascii_strtoll(iso_date, NULL, 10);
	m = (gint)g_ascii_strtoll(iso_date + 5, NULL, 10);
	d = (gint)g_ascii_strtoll(iso_date + 8, NULL, 10);
	if (!g_date_valid_dmy((GDateDay)d, (GDateMonth)m, (GDateYear)y))
		return 0;
	g_date_clear(&date, 1);
	g_date_set_dmy(&date, (GDateDay)d, (GDateMonth)m, (GDateYear)y);
	return g_date_get_julian(&date);
}

/**
 * bar_wowgold_period_days:
 * @period: (nullable): the `period' setting: "30d", "4w", "3m", "1y"
 *   or a bare number of days
 *
 * The trend window in days, for the graph.  tsmctl also accepts
 * named periods and dates for --since; those have no fixed length,
 * so they draw the default thirty days (tsmctl still gets the text).
 *
 * Returns: 2--366
 */
guint
bar_wowgold_period_days(const gchar *period)
{
	gchar *end = NULL;
	gint64 n;
	guint days;

	if (period == NULL || period[0] == '\0')
		return 30;
	n = g_ascii_strtoll(period, &end, 10);
	if (end == period || n <= 0 || n > 100000)
		return 30;
	switch (g_ascii_tolower(*end)) {
	case '\0':
	case 'd':
		days = (guint)n;
		break;
	case 'w':
		days = (guint)n * 7u;
		break;
	case 'm':
		days = (guint)n * 30u;
		break;
	case 'y':
		days = (guint)n * 365u;
		break;
	default:
		return 30;
	}
	if (*end != '\0' && end[1] != '\0')
		return 30;
	return CLAMP(days, 2u, 366u);
}

/**
 * bar_wowgold_daily_series:
 * @points: (array length=n_points): sorted, one per day
 * @n_points: how many
 * @today: the Julian day of the series' last entry
 * @days: entries wanted
 * @values: (out caller-allocates) (array length=days): copper per day
 * @known: (out caller-allocates) (array length=days): whether known
 *
 * A dense day-by-day series ending @today.  A day with no point
 * CARRIES FORWARD the last known total: TSM logs gold when a
 * character's balance changes, so a day without an entry is a day the
 * total stayed put, not a day it was zero.  Days before the first point
 * are unknown, not zero --- a "30-day change" measured against an
 * invented zero would report the whole fortune as a gain.  Points after
 * @today (a clock that ran ahead) are ignored.
 *
 * Returns: how many days are known
 */
guint
bar_wowgold_daily_series(const BarWowgoldPoint *points, guint n_points,
                         guint32 today, guint days, gint64 *values,
                         gboolean *known)
{
	guint32 start, day;
	guint i, j = 0, n_known = 0;
	gboolean have = FALSE;
	gint64 cur = 0;

	if (days == 0)
		return 0;
	start = (today >= days) ? today - (days - 1u) : 1u;
	for (i = 0; i < days; i++) {
		day = start + i;
		while (j < n_points && points[j].day <= day) {
			cur = points[j].copper;
			have = TRUE;
			j++;
		}
		values[i] = have ? cur : 0;
		known[i]  = have;
		if (have)
			n_known++;
	}
	return n_known;
}

/**
 * bar_wowgold_normalise:
 * @values: (array length=n): copper per day
 * @known: (array length=n): which are real
 * @n: how many
 * @out: (out caller-allocates) (array length=n): 0.0--1.0 each
 *
 * Scales the known range onto the graph.  The floor is 0.08 rather
 * than 0 so the lowest day is still a visible line; a flat series
 * sits mid-height; unknown leading days take the first known value so
 * the line starts flat instead of diving to the axis.
 */
void
bar_wowgold_normalise(const gint64 *values, const gboolean *known, guint n,
                      gdouble *out)
{
	gint64 lo = 0, hi = 0, first = 0;
	gboolean any = FALSE;
	guint i;

	for (i = 0; i < n; i++) {
		if (!known[i])
			continue;
		if (!any) {
			lo = hi = first = values[i];
			any = TRUE;
		}
		lo = MIN(lo, values[i]);
		hi = MAX(hi, values[i]);
	}
	for (i = 0; i < n; i++) {
		gint64 v = known[i] ? values[i] : first;

		if (!any)
			out[i] = 0.0;
		else if (hi == lo)
			out[i] = 0.5;
		else
			out[i] = 0.08 + 0.92 * ((gdouble)(v - lo) /
			                        (gdouble)(hi - lo));
	}
}

/**
 * bar_wowgold_trend:
 * @data: (nullable): a reading
 * @today: today's Julian day
 * @days: the window, at least 2
 * @out: (out caller-allocates): what the history says; release with
 *   bar_wowgold_trend_clear()
 *
 * The deltas are history against history.  The live total from
 * `get gold' is exact while TSM's gold log is rounded to the nearest
 * thousand gold, so "today minus yesterday" mixing the two would
 * report a few hundred gold of change on a day nothing happened.
 */
void
bar_wowgold_trend(const BarWowgoldData *data, guint32 today, guint days,
                  BarWowgoldTrend *out)
{
	g_autofree gint64 *values = NULL;
	g_autofree gboolean *known = NULL;
	guint last, i;

	g_return_if_fail(out != NULL);
	memset(out, 0, sizeof(*out));
	if (data == NULL || data->points->len == 0 || today == 0)
		return;

	days = MAX(days, 2u);
	values = g_new0(gint64, days);
	known  = g_new0(gboolean, days);
	if (bar_wowgold_daily_series((const BarWowgoldPoint *)data->points->data,
	                             data->points->len, today, days,
	                             values, known) == 0)
		return;

	out->n_samples = days;
	out->samples = g_new0(gdouble, days);
	bar_wowgold_normalise(values, known, days, out->samples);

	last = days - 1;
	if (known[last]) {
		out->have_today = TRUE;
		out->today = values[last];
	}
	if (known[last - 1]) {
		out->have_yesterday = TRUE;
		out->yesterday = values[last - 1];
	}
	if (out->have_today && out->have_yesterday) {
		out->have_day = TRUE;
		out->day_delta = values[last] - values[last - 1];
	}
	if (days >= 8 && known[last] && known[last - 7]) {
		out->have_week = TRUE;
		out->week_delta = values[last] - values[last - 7];
	}
	for (i = 0; i < last; i++) {
		if (known[i] && known[last]) {
			out->have_period = TRUE;
			out->period_delta = values[last] - values[i];
			break;
		}
	}
}

/**
 * bar_wowgold_trend_clear:
 * @trend: a trend filled by bar_wowgold_trend()
 */
void
bar_wowgold_trend_clear(BarWowgoldTrend *trend)
{
	if (trend == NULL)
		return;
	g_clear_pointer(&trend->samples, g_free);
	trend->n_samples = 0;
}

/**
 * bar_wowgold_direction:
 * @trend: (nullable): a trend
 *
 * Returns: up, down, or flat (which includes "no history to tell")
 */
BarWowgoldDirection
bar_wowgold_direction(const BarWowgoldTrend *trend)
{
	if (trend == NULL || !trend->have_day || trend->day_delta == 0)
		return BAR_WOWGOLD_FLAT;
	return (trend->day_delta > 0) ? BAR_WOWGOLD_UP : BAR_WOWGOLD_DOWN;
}

/* ----------------------------------------------------------------
 * Text
 * ---------------------------------------------------------------- */

/**
 * bar_wowgold_format_ago:
 * @then: unix seconds, or 0 for unknown
 * @now: unix seconds
 *
 * Returns: (transfer full): "just now", "5m ago", "3h ago", "2d ago",
 *   a date for anything older than two months, or "" for unknown
 */
gchar *
bar_wowgold_format_ago(gint64 then, gint64 now)
{
	gint64 d;

	if (then <= 0)
		return g_strdup("");
	d = now - then;
	if (d < 60)
		return g_strdup("just now");
	if (d < 3600)
		return g_strdup_printf("%" G_GINT64_FORMAT "m ago", d / 60);
	if (d < 86400)
		return g_strdup_printf("%" G_GINT64_FORMAT "h ago", d / 3600);
	if (d < 86400 * 60)
		return g_strdup_printf("%" G_GINT64_FORMAT "d ago", d / 86400);
	{
		g_autoptr(GDateTime) dt = g_date_time_new_from_unix_local(then);

		return (dt != NULL) ? g_date_time_format(dt, "%Y-%m-%d")
		                    : g_strdup("");
	}
}

/**
 * bar_wowgold_sale_title:
 * @sale: a sale
 *
 * Returns: (transfer full): "WoW Token ×2", or the bare name for one
 */
gchar *
bar_wowgold_sale_title(const BarWowgoldSale *sale)
{
	if (sale->qty > 1)
		return g_strdup_printf("%s \xc3\x97%" G_GINT64_FORMAT,
		                       sale->item, sale->qty);
	return g_strdup(sale->item);
}

/**
 * bar_wowgold_label:
 * @data: (nullable): a reading
 * @trend: (nullable): its trend
 * @compact: "35.6Mg" rather than "35,642,269g"
 * @show_delta: follow the arrow with the day's change
 * @format: the money format, for the full form
 *
 * The bar text: the total and an arrow for the day.  A flat day has
 * no arrow; its colour (SUBTEXT) says it instead, so the label does
 * not grow a glyph that means nothing.
 *
 * Returns: (transfer full) (nullable): the label, or %NULL with no total
 */
gchar *
bar_wowgold_label(const BarWowgoldData *data, const BarWowgoldTrend *trend,
                  gboolean compact, gboolean show_delta,
                  BarWowgoldMoneyFormat format)
{
	g_autofree gchar *total = NULL;
	g_autofree gchar *delta = NULL;
	BarWowgoldDirection dir;
	const gchar *arrow;

	if (data == NULL || !data->have_total)
		return NULL;
	total = compact ? bar_wowgold_format_compact(data->total)
	                : bar_wowgold_format_money(data->total, format);
	dir = bar_wowgold_direction(trend);
	if (dir == BAR_WOWGOLD_FLAT)
		return g_steal_pointer(&total);

	arrow = (dir == BAR_WOWGOLD_UP) ? "\xe2\x96\xb2" : "\xe2\x96\xbc";
	if (!show_delta)
		return g_strdup_printf("%s %s", total, arrow);

	/* The arrow already carries the sign. */
	delta = compact ? bar_wowgold_format_compact(ABS(trend->day_delta))
	                : bar_wowgold_format_money(ABS(trend->day_delta),
	                                           format);
	return g_strdup_printf("%s %s%s", total, arrow, delta);
}

/* "Today: +150,000g" or why there is no number. */
static gchar *
day_line(const BarWowgoldTrend *trend, BarWowgoldMoneyFormat format)
{
	g_autofree gchar *delta = NULL;

	if (trend == NULL || trend->n_samples == 0)
		return g_strdup("No gold history yet");
	if (!trend->have_day)
		return g_strdup("Today: no change recorded yet");
	if (trend->day_delta == 0)
		return g_strdup("Today: no change");
	delta = bar_wowgold_format_delta(trend->day_delta, format, FALSE);
	return g_strdup_printf("Today: %s", delta);
}

/**
 * bar_wowgold_tooltip:
 * @data: (nullable): a reading
 * @trend: (nullable): its trend
 * @format: the money format
 * @now: unix seconds, for "2d ago"
 *
 * Total, the day's change and the last sale, one per line.
 *
 * Returns: (transfer full): the tooltip
 */
gchar *
bar_wowgold_tooltip(const BarWowgoldData *data, const BarWowgoldTrend *trend,
                    BarWowgoldMoneyFormat format, gint64 now)
{
	GString *out;
	g_autofree gchar *total = NULL;
	g_autofree gchar *day = NULL;

	out = g_string_new(NULL);
	if (data == NULL || !data->have_total) {
		g_string_append(out, "WoW gold: no data yet");
		return g_string_free(out, FALSE);
	}
	total = bar_wowgold_format_money(data->total, format);
	day = day_line(trend, format);
	g_string_append_printf(out, "WoW gold: %s\n%s", total, day);

	if (data->sales->len > 0) {
		const BarWowgoldSale *s = g_ptr_array_index(data->sales, 0);
		g_autofree gchar *title = bar_wowgold_sale_title(s);
		g_autofree gchar *price = bar_wowgold_format_money(s->total,
		                                                   format);
		g_autofree gchar *ago = bar_wowgold_format_ago(s->when, now);

		g_string_append_printf(out, "\nLast sale: %s for %s%s%s",
			title, price, (ago[0] != '\0') ? ", " : "", ago);
	}
	return g_string_free(out, FALSE);
}

/**
 * bar_wowgold_summary:
 * @data: (nullable): a reading
 * @trend: (nullable): its trend
 * @format: the money format
 *
 * One line for the clipboard: total, day and week changes, last sale.
 * No times relative to now, because a pasted "2h ago" is wrong by the
 * time anyone reads it.
 *
 * Returns: (transfer full): the line
 */
gchar *
bar_wowgold_summary(const BarWowgoldData *data, const BarWowgoldTrend *trend,
                    BarWowgoldMoneyFormat format)
{
	GString *out;
	g_autofree gchar *total = NULL;

	if (data == NULL || !data->have_total)
		return g_strdup("WoW gold: no data");

	out = g_string_new(NULL);
	total = bar_wowgold_format_money(data->total, format);
	g_string_append_printf(out, "WoW gold: %s", total);
	if (trend != NULL && (trend->have_day || trend->have_week)) {
		g_string_append(out, " (");
		if (trend->have_day) {
			g_autofree gchar *d = bar_wowgold_format_delta(
				trend->day_delta, format, FALSE);

			g_string_append_printf(out, "today %s", d);
		}
		if (trend->have_week) {
			g_autofree gchar *w = bar_wowgold_format_delta(
				trend->week_delta, format, FALSE);

			g_string_append_printf(out, "%s7 days %s",
				trend->have_day ? ", " : "", w);
		}
		g_string_append_c(out, ')');
	}
	if (data->sales->len > 0) {
		const BarWowgoldSale *s = g_ptr_array_index(data->sales, 0);
		g_autofree gchar *title = bar_wowgold_sale_title(s);
		g_autofree gchar *price = bar_wowgold_format_money(s->total,
		                                                   format);

		g_string_append_printf(out, "; last sale %s for %s",
		                       title, price);
	}
	return g_string_free(out, FALSE);
}

/* ----------------------------------------------------------------
 * argv
 * ---------------------------------------------------------------- */

/**
 * bar_wowgold_arg_ok:
 * @value: (nullable): a setting headed for tsmctl's argv
 *
 * Non-empty, valid UTF-8, no control characters, at most 128 bytes.
 * A leading `-' is allowed: every value goes in `--opt=VALUE' form, so
 * argparse cannot take it for an option.
 *
 * Returns: whether @value may be passed
 */
gboolean
bar_wowgold_arg_ok(const gchar *value)
{
	const guchar *p;

	if (value == NULL || value[0] == '\0' || strlen(value) > 128 ||
	    !g_utf8_validate(value, -1, NULL))
		return FALSE;
	for (p = (const guchar *)value; *p != '\0'; p++)
		if (*p < 0x20u || *p == 0x7Fu)
			return FALSE;
	return TRUE;
}

/**
 * bar_wowgold_host_ok:
 * @host: (nullable): the `host' setting
 *
 * An ssh destination: letters, digits and `.-_@:%[]', not starting
 * with `-'.  tsmctl hands its host to ssh as a bare argument, and
 * "-oProxyCommand=..." there is not a host, it is a command.
 *
 * Returns: whether @host is a plausible destination
 */
gboolean
bar_wowgold_host_ok(const gchar *host)
{
	const gchar *p;

	if (!bar_wowgold_arg_ok(host) || host[0] == '-')
		return FALSE;
	for (p = host; *p != '\0'; p++) {
		if (!g_ascii_isalnum(*p) && strchr(".-_@:%[]", *p) == NULL)
			return FALSE;
	}
	return TRUE;
}

/* `~/x' -> `$HOME/x' for the program word, as a shell would. */
static gchar *
expand_tilde(const gchar *word)
{
	if (g_strcmp0(word, "~") == 0)
		return g_strdup(g_get_home_dir());
	if (g_str_has_prefix(word, "~/"))
		return g_build_filename(g_get_home_dir(), word + 2, NULL);
	return g_strdup(word);
}

/* One `--flag=VALUE' per comma-separated entry of @list. */
static gboolean
push_list(GPtrArray *argv, const gchar *flag, const gchar *setting,
          const gchar *list, GError **error)
{
	g_auto(GStrv) parts = NULL;
	guint i;

	if (list == NULL || list[0] == '\0')
		return TRUE;
	parts = g_strsplit(list, ",", -1);
	for (i = 0; parts[i] != NULL; i++) {
		g_strstrip(parts[i]);
		if (parts[i][0] == '\0')
			continue;
		if (!bar_wowgold_arg_ok(parts[i])) {
			g_set_error(error, BAR_WOWGOLD_ERROR, 0,
			            "the %s setting has an unusable entry", setting);
			return FALSE;
		}
		g_ptr_array_add(argv, g_strdup_printf("%s=%s", flag, parts[i]));
	}
	return TRUE;
}

/**
 * bar_wowgold_build_argv:
 * @args: the settings
 * @command: which question
 * @error: (out) (optional): why there is no argv
 *
 * The argv for one tsmctl call.  It is always run directly (never via
 * a shell), every value is attached as `--opt=VALUE', and the shape is
 *
 *   tsmctl [--local | --host=H] [--offline] [--flavor=F] [--account=A]...
 *          [--realm=R]... [--char=C]... --quiet --color=never
 *          --output=json <command words>
 *
 * The data-source and filter options go before the command because
 * tsmctl accepts them there for every command (its subparsers default
 * them to SUPPRESS so the earlier value stands).  The report is the one
 * command that is not JSON: it is for a person, in colour.
 *
 * Returns: (transfer full) (nullable): a %NULL-terminated argv, or %NULL
 *   with @error set when a setting cannot be passed safely
 */
gchar **
bar_wowgold_build_argv(const BarWowgoldArgs *args, BarWowgoldCommand command,
                       GError **error)
{
	g_autoptr(GPtrArray) argv = NULL;
	g_auto(GStrv) words = NULL;
	const gchar *cmd, *period;
	gboolean local = FALSE;
	GError *parse_error = NULL;
	gint n = 0, i;

	g_return_val_if_fail(args != NULL, NULL);

	cmd = (args->command != NULL && args->command[0] != '\0')
	      ? args->command : "tsmctl";
	if (!g_shell_parse_argv(cmd, &n, &words, &parse_error)) {
		g_set_error(error, BAR_WOWGOLD_ERROR, 0,
		            "tsmctl-command cannot be parsed: %s",
		            parse_error->message);
		g_clear_error(&parse_error);
		return NULL;
	}

	argv = g_ptr_array_new_with_free_func(g_free);
	for (i = 0; i < n; i++)
		g_ptr_array_add(argv, (i == 0) ? expand_tilde(words[i])
		                               : g_strdup(words[i]));

	/* Where the data comes from. */
	if (args->host != NULL && args->host[0] != '\0') {
		if (g_ascii_strcasecmp(args->host, "local") == 0) {
			g_ptr_array_add(argv, g_strdup("--local"));
			local = TRUE;
		} else if (bar_wowgold_host_ok(args->host)) {
			g_ptr_array_add(argv,
				g_strdup_printf("--host=%s", args->host));
		} else {
			g_set_error(error, BAR_WOWGOLD_ERROR,
			            0,
			            "the host setting is not an ssh host name");
			return NULL;
		}
	}
	if (args->offline && !local)
		g_ptr_array_add(argv, g_strdup("--offline"));

	if (args->flavor != NULL && args->flavor[0] != '\0') {
		if (!bar_wowgold_arg_ok(args->flavor)) {
			g_set_error(error, BAR_WOWGOLD_ERROR,
			            0,
			            "the flavor setting is unusable");
			return NULL;
		}
		g_ptr_array_add(argv,
			g_strdup_printf("--flavor=%s", args->flavor));
	}
	if (!push_list(argv, "--account", "account", args->account, error) ||
	    !push_list(argv, "--realm", "realm", args->realm, error) ||
	    !push_list(argv, "--char", "char", args->character, error))
		return NULL;

	g_ptr_array_add(argv, g_strdup("--quiet"));
	if (command == BAR_WOWGOLD_CMD_REPORT) {
		g_ptr_array_add(argv, g_strdup("--color=always"));
	} else {
		g_ptr_array_add(argv, g_strdup("--color=never"));
		g_ptr_array_add(argv, g_strdup("--output=json"));
	}

	switch (command) {
	case BAR_WOWGOLD_CMD_GOLD:
		g_ptr_array_add(argv, g_strdup("get"));
		g_ptr_array_add(argv, g_strdup("gold"));
		break;
	case BAR_WOWGOLD_CMD_HISTORY:
		period = (args->period != NULL && args->period[0] != '\0')
		         ? args->period : "30d";
		if (!bar_wowgold_arg_ok(period)) {
			g_set_error(error, BAR_WOWGOLD_ERROR,
			            0,
			            "the period setting is unusable");
			return NULL;
		}
		g_ptr_array_add(argv, g_strdup("get"));
		g_ptr_array_add(argv, g_strdup("gold-history"));
		g_ptr_array_add(argv, g_strdup("--by=day"));
		g_ptr_array_add(argv, g_strdup_printf("--since=%s", period));
		break;
	case BAR_WOWGOLD_CMD_SALES:
		g_ptr_array_add(argv, g_strdup("get"));
		g_ptr_array_add(argv, g_strdup("sales"));
		g_ptr_array_add(argv, g_strdup_printf("--limit=%u",
			CLAMP(args->sales_count, 1u, 100u)));
		break;
	case BAR_WOWGOLD_CMD_PNL:
		g_ptr_array_add(argv, g_strdup("pnl"));
		g_ptr_array_add(argv, g_strdup("--period=today"));
		break;
	case BAR_WOWGOLD_CMD_ATTENTION:
		g_ptr_array_add(argv, g_strdup("attention"));
		break;
	case BAR_WOWGOLD_CMD_REPORT:
	default:
		g_ptr_array_add(argv, g_strdup("report"));
		break;
	}

	g_ptr_array_add(argv, NULL);
	return (gchar **)g_ptr_array_free(g_steal_pointer(&argv), FALSE);
}

/**
 * bar_wowgold_fetch_key:
 * @args: the settings
 *
 * Everything that decides WHAT is fetched, as one string, so a change
 * of host or filter can be told from a change of colour: the first
 * means the data on screen is now someone else's and must be fetched
 * again at once.
 *
 * Returns: (transfer full): the key
 */
gchar *
bar_wowgold_fetch_key(const BarWowgoldArgs *args)
{
	return g_strdup_printf("%s\x1f%s\x1f%s\x1f%s\x1f%s\x1f%s\x1f%s\x1f%u",
		args->command   != NULL ? args->command   : "",
		args->host      != NULL ? args->host      : "",
		args->realm     != NULL ? args->realm     : "",
		args->character != NULL ? args->character : "",
		args->account   != NULL ? args->account   : "",
		args->flavor    != NULL ? args->flavor    : "",
		args->period    != NULL ? args->period    : "",
		args->sales_count);
}
