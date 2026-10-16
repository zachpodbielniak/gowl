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
 * The webmail widget's pure half.  See bar-webmail-model.h for why it
 * is separate; everything here takes plain values and returns plain
 * values, so tests/test-bar-webmail.c includes this file directly.
 *
 * Nothing in here may touch a GowlBarPlugin, a theme, the compositor
 * or a subprocess.  Colours do appear as hex here, and that is the
 * point: this is where a palette that was resolved from theme ROLES
 * becomes the literal CSS a web page needs.  The bar side still names
 * roles only (source guard 4 holds bar-plugins-*.c to that).
 */

#include <string.h>
#include <stdlib.h>
#include <math.h>

#include "bar-webmail-model.h"

/* ================================================================
 * Titles
 * ================================================================ */

/* The largest count we will believe; anything bigger is "lots" and
   is reported with @plus set rather than as a number nobody can use. */
#define BAR_WEBMAIL_MAX_COUNT (99999)

/*
 * Words a web mail puts before "(N)" in its title for the inbox, case
 * folded.  Gmail and Proton write `Inbox (3)'; a localised Gmail
 * writes the folder in the UI language, which is why this is a list
 * and not one word.  Matched only as a whole word followed by the
 * count, so a subject line such as "Budget (2024)" is never read as
 * two thousand unread messages.
 */
static const gchar *const inbox_words[] = {
	"inbox", "posteingang", "boîte de réception", "bandeja de entrada",
	"posta in arrivo", "caixa de entrada", "postvak in", "inkorgen",
	"indbakke", "innboks", "saapuneet", "odebrane", "doručená pošta",
	"входящие", "受信トレイ", "收件箱", "받은편지함",
	NULL
};

/*
 * Skip whitespace and invisible formatting characters (bidi marks,
 * zero-width joiners).  A right-to-left account name makes some sites
 * start the title with U+200F, and the count must still be found.
 */
static const gchar *
skip_blank(const gchar *p)
{
	while (*p != '\0') {
		gunichar c = g_utf8_get_char(p);

		if (!g_unichar_isspace(c) &&
		    g_unichar_type(c) != G_UNICODE_FORMAT)
			break;
		p = g_utf8_next_char(p);
	}
	return p;
}

/*
 * Parse "N)" or "N+)" at @p (the opening parenthesis already
 * consumed).  Any Unicode decimal digit counts, so a title localised
 * with Arabic-Indic digits still works; `,' `.' and the no-break
 * spaces are accepted between digits as thousands separators.
 *
 * Returns the count, or -1 when @p is not a count.
 */
static gint
parse_count(const gchar *p, gboolean *plus)
{
	gint64 value = 0;
	gboolean any = FALSE;
	gboolean big = FALSE;

	p = skip_blank(p);
	while (*p != '\0') {
		gunichar c = g_utf8_get_char(p);
		gint d = g_unichar_digit_value(c);

		if (d >= 0) {
			any = TRUE;
			if (value <= BAR_WEBMAIL_MAX_COUNT)
				value = value * 10 + d;
			if (value > BAR_WEBMAIL_MAX_COUNT)
				big = TRUE;
			p = g_utf8_next_char(p);
			continue;
		}
		if (any && (c == ',' || c == '.' || c == 0x00a0 ||
		            c == 0x202f)) {
			const gchar *next = g_utf8_next_char(p);

			/* A separator only between digits; "3.)" is not. */
			if (g_unichar_digit_value(g_utf8_get_char(next)) >= 0) {
				p = next;
				continue;
			}
		}
		break;
	}
	if (!any)
		return -1;
	p = skip_blank(p);
	if (*p == '+') {
		big = TRUE;
		p = skip_blank(p + 1);
	}
	if (*p != ')')
		return -1;
	if (plus != NULL)
		*plus = big;
	return (gint)MIN(value, (gint64)BAR_WEBMAIL_MAX_COUNT);
}

/**
 * bar_webmail_parse_unread:
 * @title: (nullable): a window title
 * @plus: (out) (optional): set when the count was capped ("999+")
 *
 * Reads an unread count out of a web mail window's title.
 *
 * Two shapes are recognised, and only these two:
 *
 *  - a leading "(N)": what the widget's own userscript writes for
 *    Outlook, and what many sites (Roundcube, chat-style apps) write
 *    themselves;
 *  - an inbox word followed by "(N)" anywhere: Gmail's
 *    "Inbox (3) - me@gmail.com - Gmail", Proton's "Inbox (3) | ...".
 *
 * Anything else -- a subject line with a year in brackets, a bare
 * number -- is not a count.  A wrong number in the bar is worse than
 * none.
 *
 * Returns: the count (0 for an explicit "(0)"), or -1 for none
 */
gint
bar_webmail_parse_unread(
	const gchar *title,
	gboolean    *plus
){
	g_autofree gchar *folded = NULL;
	const gchar *p;
	guint i;
	gint n;

	if (plus != NULL)
		*plus = FALSE;
	if (title == NULL || !g_utf8_validate(title, -1, NULL))
		return -1;

	/* Shape one: "(N) anything". */
	p = skip_blank(title);
	if (*p == '(') {
		n = parse_count(p + 1, plus);
		if (n >= 0)
			return n;
	}

	/* Shape two: "<inbox> (N)", case-insensitively.  Case folding
	   can change byte lengths, so search the folded copy and parse
	   from there too -- digits and parentheses fold to themselves. */
	folded = g_utf8_casefold(title, -1);
	for (i = 0; inbox_words[i] != NULL; i++) {
		const gchar *hit = folded;
		gsize wlen = strlen(inbox_words[i]);

		while ((hit = strstr(hit, inbox_words[i])) != NULL) {
			const gchar *after = skip_blank(hit + wlen);
			gboolean word_start;

			/* A whole word: not the tail of "Sandboxinbox". */
			word_start = (hit == folded) ||
				!g_unichar_isalnum(g_utf8_get_char(
					g_utf8_prev_char(hit)));
			if (word_start && *after == '(') {
				n = parse_count(after + 1, plus);
				if (n >= 0)
					return n;
			}
			hit += wlen;
		}
	}
	return -1;
}

/* ================================================================
 * URLs
 * ================================================================ */

/**
 * bar_webmail_url_host:
 * @url: (nullable): a URL
 *
 * Returns: (transfer full) (nullable): the URL's host, lower-cased,
 *   or %NULL when @url has none
 */
gchar *
bar_webmail_url_host(const gchar *url)
{
	g_autoptr(GUri) uri = NULL;
	const gchar *host;

	if (url == NULL || *url == '\0')
		return NULL;
	uri = g_uri_parse(url, G_URI_FLAGS_NONE, NULL);
	if (uri == NULL)
		return NULL;
	host = g_uri_get_host(uri);
	if (host == NULL || *host == '\0')
		return NULL;
	return g_ascii_strdown(host, -1);
}

/* @host is @domain or a subdomain of it. */
static gboolean
host_is(const gchar *host, const gchar *domain)
{
	gsize hl, dl;

	if (host == NULL)
		return FALSE;
	if (g_strcmp0(host, domain) == 0)
		return TRUE;
	hl = strlen(host);
	dl = strlen(domain);
	return hl > dl + 1 && host[hl - dl - 1] == '.' &&
	       strcmp(host + hl - dl, domain) == 0;
}

/* Every host Outlook on the web is served from.  A work account may
   land on any of the first three; a personal one on the last. */
static const gchar *const outlook_hosts[] = {
	"outlook.cloud.microsoft", "outlook.office.com",
	"outlook.office365.com", "outlook.live.com", NULL
};

/**
 * bar_webmail_provider:
 * @url: (nullable): the configured mail URL
 *
 * Returns: which web mail @url is, %BAR_WEBMAIL_PROVIDER_GENERIC when
 *   it is none the widget knows by name
 */
BarWebmailProvider
bar_webmail_provider(const gchar *url)
{
	g_autofree gchar *host = bar_webmail_url_host(url);
	guint i;

	if (host == NULL)
		return BAR_WEBMAIL_PROVIDER_GENERIC;
	for (i = 0; outlook_hosts[i] != NULL; i++)
		if (host_is(host, outlook_hosts[i]))
			return BAR_WEBMAIL_PROVIDER_OUTLOOK;
	if (host_is(host, "mail.google.com"))
		return BAR_WEBMAIL_PROVIDER_GMAIL;
	if (host_is(host, "fastmail.com"))
		return BAR_WEBMAIL_PROVIDER_FASTMAIL;
	if (host_is(host, "proton.me") || host_is(host, "protonmail.com"))
		return BAR_WEBMAIL_PROVIDER_PROTON;
	return BAR_WEBMAIL_PROVIDER_GENERIC;
}

/**
 * bar_webmail_provider_name:
 * @provider: a provider
 *
 * Returns: (transfer none): its display name
 */
const gchar *
bar_webmail_provider_name(BarWebmailProvider provider)
{
	switch (provider) {
	case BAR_WEBMAIL_PROVIDER_OUTLOOK:  return "Outlook";
	case BAR_WEBMAIL_PROVIDER_GMAIL:    return "Gmail";
	case BAR_WEBMAIL_PROVIDER_FASTMAIL: return "Fastmail";
	case BAR_WEBMAIL_PROVIDER_PROTON:   return "Proton Mail";
	case BAR_WEBMAIL_PROVIDER_GENERIC:
	default:                            return "Web mail";
	}
}

/**
 * bar_webmail_compose_url:
 * @url: (nullable): the configured mail URL
 *
 * The page that opens a blank message, for the providers that have a
 * stable one.  Built on the configured URL's own scheme and host, so
 * an Outlook account served from outlook.office.com composes there
 * rather than being bounced through a different tenant's login.
 *
 * Returns: (transfer full) (nullable): the compose URL, or %NULL for a
 *   provider with no known deep link (set `compose-url')
 */
gchar *
bar_webmail_compose_url(const gchar *url)
{
	g_autoptr(GUri) uri = NULL;
	g_autofree gchar *host = NULL;
	const gchar *scheme;

	if (url == NULL)
		return NULL;
	uri = g_uri_parse(url, G_URI_FLAGS_NONE, NULL);
	if (uri == NULL || g_uri_get_host(uri) == NULL)
		return NULL;
	host = g_ascii_strdown(g_uri_get_host(uri), -1);
	scheme = (g_uri_get_scheme(uri) != NULL) ? g_uri_get_scheme(uri)
	                                         : "https";

	switch (bar_webmail_provider(url)) {
	case BAR_WEBMAIL_PROVIDER_OUTLOOK:
		return g_strdup_printf("%s://%s/mail/deeplink/compose",
		                       scheme, host);
	case BAR_WEBMAIL_PROVIDER_GMAIL:
		return g_strdup_printf("%s://%s/mail/?view=cm&fs=1",
		                       scheme, host);
	case BAR_WEBMAIL_PROVIDER_FASTMAIL:
		return g_strdup_printf("%s://%s/action/compose/",
		                       scheme, host);
	case BAR_WEBMAIL_PROVIDER_PROTON:
		return g_strdup_printf("%s://%s/inbox#mailto=",
		                       scheme, host);
	case BAR_WEBMAIL_PROVIDER_GENERIC:
	default:
		return NULL;
	}
}

/**
 * bar_webmail_style_patterns:
 * @url: (nullable): the configured mail URL
 *
 * The WebKit URL patterns the generated stylesheet is limited to.
 *
 * Scoped rather than global so the sign-in pages in between (Microsoft
 * and Google accounts are other hosts entirely) keep their own colours
 * instead of getting half a theme with unreadable text.  Outlook gets
 * every host it is served from, since a tenant can move between them.
 *
 * Returns: (transfer full) (nullable) (array zero-terminated=1): the
 *   patterns, or %NULL when @url has no host
 */
gchar **
bar_webmail_style_patterns(const gchar *url)
{
	g_autofree gchar *host = bar_webmail_url_host(url);
	GPtrArray *out;
	guint i;

	if (host == NULL)
		return NULL;
	out = g_ptr_array_new();
	if (bar_webmail_provider(url) == BAR_WEBMAIL_PROVIDER_OUTLOOK) {
		gboolean listed = FALSE;

		for (i = 0; outlook_hosts[i] != NULL; i++) {
			g_ptr_array_add(out, g_strdup_printf("*://%s/*",
			                                     outlook_hosts[i]));
			if (g_strcmp0(host, outlook_hosts[i]) == 0)
				listed = TRUE;
		}
		if (!listed)
			g_ptr_array_add(out, g_strdup_printf("*://%s/*", host));
	} else {
		g_ptr_array_add(out, g_strdup_printf("*://%s/*", host));
	}
	g_ptr_array_add(out, NULL);
	return (gchar **)g_ptr_array_free(out, FALSE);
}

/* ================================================================
 * Commands
 * ================================================================ */

/* Replace every @needle in @s with @value. */
static gchar *
substitute(const gchar *s, const gchar *needle, const gchar *value,
           gboolean *found)
{
	GString *out;
	const gchar *p, *hit;
	gsize nl = strlen(needle);

	out = g_string_new(NULL);
	p = s;
	while ((hit = strstr(p, needle)) != NULL) {
		g_string_append_len(out, p, (gssize)(hit - p));
		g_string_append(out, value);
		p = hit + nl;
		if (found != NULL)
			*found = TRUE;
	}
	g_string_append(out, p);
	return g_string_free(out, FALSE);
}

/**
 * bar_webmail_build_argv:
 * @template_: (nullable): the `browser-command' setting
 * @config_path: the generated gsurf configuration
 * @url: the page to open
 * @error: (out) (optional): why the command could not be built
 *
 * Turns the command template into an argv.
 *
 * The template is TOKENISED FIRST and the placeholders are filled in
 * afterwards, inside the tokens.  That ordering is the whole point: a
 * URL is full of `&', `?', `#' and sometimes spaces, and substituting
 * it into the string before splitting would let it become several
 * arguments, or a shell would run the half after the `&'.  No shell is
 * ever involved.  A template without {url} gets the URL appended.
 *
 * A URL starting with `-' is refused: as the last argument it would be
 * parsed by the browser as an option.
 *
 * Returns: (transfer full) (nullable) (array zero-terminated=1): the
 *   argv, or %NULL with @error set
 */
gchar **
bar_webmail_build_argv(
	const gchar  *template_,
	const gchar  *config_path,
	const gchar  *url,
	GError      **error
){
	g_auto(GStrv) tokens = NULL;
	GPtrArray *argv;
	gboolean saw_url = FALSE;
	gint argc = 0;
	gint i;

	if (template_ == NULL || *template_ == '\0')
		template_ = "gsurf --kiosk --no-c-config -c {config} {url}";
	if (url == NULL || *url == '\0' || url[0] == '-') {
		g_set_error_literal(error, G_SHELL_ERROR, G_SHELL_ERROR_FAILED,
		                    "the mail URL is empty or starts with '-'");
		return NULL;
	}
	if (!g_shell_parse_argv(template_, &argc, &tokens, error))
		return NULL;

	argv = g_ptr_array_new_with_free_func(g_free);
	for (i = 0; i < argc; i++) {
		g_autofree gchar *with_config = NULL;

		with_config = substitute(tokens[i], "{config}",
			(config_path != NULL) ? config_path : "", NULL);
		g_ptr_array_add(argv, substitute(with_config, "{url}", url,
		                                 &saw_url));
	}
	if (!saw_url)
		g_ptr_array_add(argv, g_strdup(url));
	g_ptr_array_add(argv, NULL);
	return (gchar **)g_ptr_array_free(argv, FALSE);
}

/**
 * bar_webmail_cmdline_has_arg:
 * @cmdline: the contents of /proc/<pid>/cmdline (NUL-separated)
 * @len: its length in bytes
 * @needle: an argument to look for
 *
 * Whether a process was given exactly @needle as one of its
 * arguments.  The widget asks this before trusting a remembered PID
 * -- PIDs are reused, and signalling one that now belongs to somebody
 * else's process is the pattern-kill mistake with extra steps.
 *
 * Returns: %TRUE when one argument equals @needle
 */
gboolean
bar_webmail_cmdline_has_arg(
	const gchar *cmdline,
	gsize        len,
	const gchar *needle
){
	gsize pos = 0;

	if (cmdline == NULL || needle == NULL || *needle == '\0')
		return FALSE;
	while (pos < len) {
		const gchar *arg = cmdline + pos;
		gsize alen = strnlen(arg, len - pos);

		if (alen == strlen(needle) && memcmp(arg, needle, alen) == 0)
			return TRUE;
		/* An argument may also carry it after `=': `--config=PATH'. */
		{
			const gchar *eq = memchr(arg, '=', alen);

			if (eq != NULL &&
			    (gsize)(arg + alen - eq - 1) == strlen(needle) &&
			    memcmp(eq + 1, needle, strlen(needle)) == 0)
				return TRUE;
		}
		pos += alen + 1;
	}
	return FALSE;
}

/**
 * bar_webmail_stat_ppid:
 * @stat_text: (nullable): the contents of /proc/<pid>/stat
 *
 * The parent PID from a stat line.  The command name in brackets may
 * itself contain spaces and `)', so the fields are counted from the
 * LAST closing bracket, as procps does.
 *
 * Returns: the parent PID, or -1 when the line does not parse
 */
pid_t
bar_webmail_stat_ppid(const gchar *stat_text)
{
	const gchar *p;
	gchar *end = NULL;
	gint64 v;

	if (stat_text == NULL)
		return -1;
	p = strrchr(stat_text, ')');
	if (p == NULL)
		return -1;
	p++;
	while (*p == ' ')
		p++;
	if (*p == '\0')
		return -1;
	p++;               /* the state letter */
	while (*p == ' ')
		p++;
	v = g_ascii_strtoll(p, &end, 10);
	if (end == p || v < 0 || v > G_MAXINT32)
		return -1;
	return (pid_t)v;
}

/* ================================================================
 * Colour
 * ================================================================ */

static gint
hexval(gchar c)
{
	return g_ascii_xdigit_value(c);
}

/**
 * bar_webmail_parse_hex:
 * @spec: (nullable): `#rgb', `#rrggbb' or `#rrggbbaa'
 * @out: (out): the colour; alpha is ignored, a page ground is opaque
 *
 * Returns: %TRUE when @spec is one of those spellings exactly
 */
gboolean
bar_webmail_parse_hex(const gchar *spec, BarWebmailRgb *out)
{
	gsize len, i;
	gint v[8];

	if (spec == NULL || spec[0] != '#')
		return FALSE;
	len = strlen(spec + 1);
	if (len != 3 && len != 6 && len != 8)
		return FALSE;
	for (i = 0; i < len; i++) {
		v[i] = hexval(spec[1 + i]);
		if (v[i] < 0)
			return FALSE;
	}
	if (len == 3) {
		out->r = (gdouble)(v[0] * 17) / 255.0;
		out->g = (gdouble)(v[1] * 17) / 255.0;
		out->b = (gdouble)(v[2] * 17) / 255.0;
	} else {
		out->r = (gdouble)(v[0] * 16 + v[1]) / 255.0;
		out->g = (gdouble)(v[2] * 16 + v[3]) / 255.0;
		out->b = (gdouble)(v[4] * 16 + v[5]) / 255.0;
	}
	return TRUE;
}

static gdouble
linear(gdouble c)
{
	return (c <= 0.04045) ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
}

/**
 * bar_webmail_luminance:
 * @c: a colour
 *
 * Relative luminance as WCAG defines it: 0 is black, 1 is white.
 *
 * Returns: the luminance
 */
gdouble
bar_webmail_luminance(const BarWebmailRgb *c)
{
	return 0.2126 * linear(c->r) + 0.7152 * linear(c->g) +
	       0.0722 * linear(c->b);
}

/* WCAG contrast ratio, 1..21. */
static gdouble
contrast(const BarWebmailRgb *a, const BarWebmailRgb *b)
{
	gdouble la = bar_webmail_luminance(a);
	gdouble lb = bar_webmail_luminance(b);

	return (MAX(la, lb) + 0.05) / (MIN(la, lb) + 0.05);
}

/**
 * bar_webmail_palette_dark:
 * @p: a palette
 *
 * Whether the palette is a dark one, judged by its background.
 *
 * Luminance rather than the palette's name, so a hand-written palette
 * and a per-widget `color-background' override are judged by what they
 * actually look like.  The cut is a perceptual midpoint (0.18 linear
 * is mid-grey, L* 50), not 0.5: Catppuccin Latte's base is 0.89 and
 * Mocha's 0.016, and a palette in between should go dark only once
 * it really looks dark.  With no background, a light TEXT colour
 * means a dark theme.
 *
 * Returns: 1 dark, 0 light, -1 when the palette says nothing
 */
gint
bar_webmail_palette_dark(const BarWebmailPalette *p)
{
	if (p->set[BAR_WEBMAIL_PART_BACKGROUND])
		return bar_webmail_luminance(
			&p->colors[BAR_WEBMAIL_PART_BACKGROUND]) < 0.18 ? 1 : 0;
	if (p->set[BAR_WEBMAIL_PART_TEXT])
		return bar_webmail_luminance(
			&p->colors[BAR_WEBMAIL_PART_TEXT]) >= 0.18 ? 1 : 0;
	return -1;
}

/* `#rrggbb', rounded and clamped. */
static void
append_hex(GString *out, const BarWebmailRgb *c)
{
	g_string_append_printf(out, "#%02x%02x%02x",
		(guint)CLAMP((gint)lround(c->r * 255.0), 0, 255),
		(guint)CLAMP((gint)lround(c->g * 255.0), 0, 255),
		(guint)CLAMP((gint)lround(c->b * 255.0), 0, 255));
}

static const gchar *const part_names[BAR_WEBMAIL_N_PARTS] = {
	"background", "surface", "text", "subtext", "accent", "border"
};

/**
 * bar_webmail_part_name:
 * @part: a part
 *
 * Returns: (transfer none): its setting suffix, as in `color-<name>'
 */
const gchar *
bar_webmail_part_name(BarWebmailPart part)
{
	if ((guint)part >= BAR_WEBMAIL_N_PARTS)
		return "";
	return part_names[part];
}

/**
 * bar_webmail_palette_signature:
 * @p: a palette
 *
 * A string that changes exactly when the CSS would.  The plugin
 * compares it every poll to notice a palette switch without diffing
 * stylesheets.
 *
 * Returns: (transfer full): the signature
 */
gchar *
bar_webmail_palette_signature(const BarWebmailPalette *p)
{
	GString *out = g_string_new(NULL);
	guint i;

	for (i = 0; i < BAR_WEBMAIL_N_PARTS; i++) {
		g_string_append_printf(out, "%s=", part_names[i]);
		if (p->set[i])
			append_hex(out, &p->colors[i]);
		else
			g_string_append_c(out, '-');
		g_string_append_c(out, ';');
	}
	return g_string_free(out, FALSE);
}

/* ================================================================
 * The stylesheet
 * ================================================================ */

/*
 * How one CSS value is made from the palette.
 *
 * Every Fluent token is one of the six parts or a blend of two, so a
 * table of recipes is the whole theme.  A recipe whose parts are not
 * all set yields nothing and its declaration is left out, which is
 * what lets `follow-theme: false' plus a single override theme just
 * that one thing.
 */
typedef enum {
	MIX_PART = 0,   /* a */
	MIX_BLEND,      /* a toward b by t */
	MIX_DARKEN,     /* a toward black by t */
	MIX_ON          /* text or background, whichever reads on a */
} MixOp;

typedef struct {
	const gchar   *name;
	MixOp          op;
	BarWebmailPart a;
	BarWebmailPart b;
	gdouble        t;
} Recipe;

#define BG   BAR_WEBMAIL_PART_BACKGROUND
#define SURF BAR_WEBMAIL_PART_SURFACE
#define TXT  BAR_WEBMAIL_PART_TEXT
#define SUB  BAR_WEBMAIL_PART_SUBTEXT
#define ACC  BAR_WEBMAIL_PART_ACCENT
#define BRD  BAR_WEBMAIL_PART_BORDER

#define P(n, a)        { n, MIX_PART,   a, a, 0.0 }
#define B(n, a, b, t)  { n, MIX_BLEND,  a, b, t }
#define K(n, a, t)     { n, MIX_DARKEN, a, a, t }
#define ON(n, a)       { n, MIX_ON,     a, a, 0.0 }

/*
 * Fluent UI v9's colour tokens, which Outlook on the web (2023 and
 * later) reads everywhere through CSS custom properties.  Background1
 * is the main surface; 2..5 step darker in both of Fluent's own light
 * and dark themes, and 6 is the raised one -- mirrored here with
 * DARKEN steps and SURFACE.
 */
static const Recipe fluent_tokens[] = {
	P("--colorNeutralForeground1", TXT),
	P("--colorNeutralForeground1Hover", TXT),
	P("--colorNeutralForeground1Pressed", TXT),
	P("--colorNeutralForeground1Selected", TXT),
	P("--colorNeutralForeground2", SUB),
	P("--colorNeutralForeground2Hover", TXT),
	P("--colorNeutralForeground2Pressed", TXT),
	P("--colorNeutralForeground2Selected", TXT),
	P("--colorNeutralForeground2BrandHover", ACC),
	P("--colorNeutralForeground2BrandPressed", ACC),
	P("--colorNeutralForeground2BrandSelected", ACC),
	B("--colorNeutralForeground3", SUB, BG, 0.20),
	P("--colorNeutralForeground3Hover", SUB),
	P("--colorNeutralForeground3Pressed", SUB),
	P("--colorNeutralForeground3Selected", SUB),
	B("--colorNeutralForeground4", SUB, BG, 0.35),
	B("--colorNeutralForegroundDisabled", TXT, BG, 0.60),
	ON("--colorNeutralForegroundOnBrand", ACC),
	P("--colorNeutralForegroundInverted", BG),
	P("--colorBrandForegroundLink", ACC),
	P("--colorBrandForegroundLinkHover", ACC),
	P("--colorBrandForegroundLinkPressed", ACC),
	P("--colorBrandForegroundLinkSelected", ACC),
	P("--colorBrandForeground1", ACC),
	B("--colorBrandForeground2", ACC, TXT, 0.20),
	P("--colorCompoundBrandForeground1", ACC),
	B("--colorCompoundBrandForeground1Hover", ACC, TXT, 0.15),
	B("--colorCompoundBrandForeground1Pressed", ACC, BG, 0.25),
	P("--colorNeutralBackground1", BG),
	B("--colorNeutralBackground1Hover", BG, SURF, 0.50),
	P("--colorNeutralBackground1Pressed", SURF),
	P("--colorNeutralBackground1Selected", SURF),
	K("--colorNeutralBackground2", BG, 0.06),
	B("--colorNeutralBackground2Hover", BG, SURF, 0.50),
	P("--colorNeutralBackground2Pressed", SURF),
	P("--colorNeutralBackground2Selected", SURF),
	K("--colorNeutralBackground3", BG, 0.12),
	B("--colorNeutralBackground3Hover", BG, SURF, 0.50),
	P("--colorNeutralBackground3Pressed", SURF),
	P("--colorNeutralBackground3Selected", SURF),
	K("--colorNeutralBackground4", BG, 0.18),
	K("--colorNeutralBackground5", BG, 0.24),
	P("--colorNeutralBackground6", SURF),
	P("--colorNeutralBackgroundInverted", TXT),
	B("--colorNeutralBackgroundDisabled", BG, SURF, 0.50),
	B("--colorSubtleBackgroundHover", BG, SURF, 0.50),
	P("--colorSubtleBackgroundPressed", SURF),
	P("--colorSubtleBackgroundSelected", SURF),
	P("--colorNeutralStroke1", BRD),
	B("--colorNeutralStroke1Hover", BRD, TXT, 0.20),
	B("--colorNeutralStroke1Pressed", BRD, TXT, 0.30),
	B("--colorNeutralStroke1Selected", BRD, TXT, 0.20),
	B("--colorNeutralStroke2", BRD, BG, 0.35),
	B("--colorNeutralStroke3", BRD, BG, 0.55),
	B("--colorNeutralStrokeSubtle", BRD, BG, 0.50),
	P("--colorNeutralStrokeAccessible", SUB),
	P("--colorNeutralStrokeAccessibleHover", TXT),
	P("--colorBrandBackground", ACC),
	B("--colorBrandBackgroundHover", ACC, TXT, 0.15),
	B("--colorBrandBackgroundPressed", ACC, BG, 0.25),
	B("--colorBrandBackgroundSelected", ACC, TXT, 0.10),
	B("--colorBrandBackground2", ACC, BG, 0.80),
	P("--colorCompoundBrandBackground", ACC),
	B("--colorCompoundBrandBackgroundHover", ACC, TXT, 0.15),
	B("--colorCompoundBrandBackgroundPressed", ACC, BG, 0.25),
	P("--colorBrandStroke1", ACC),
	B("--colorBrandStroke2", ACC, BG, 0.60),
	P("--colorCompoundBrandStroke", ACC),
	B("--colorCompoundBrandStrokeHover", ACC, TXT, 0.15),
	P("--colorStrokeFocus2", ACC),
	{ NULL, MIX_PART, BG, BG, 0.0 }
};

/*
 * The older Fluent v8 (Office UI Fabric) theme slots.  Parts of
 * Outlook -- the reading pane's chrome, some dialogs -- still read
 * these, and without them a themed window has light islands in it.
 */
static const Recipe fabric_tokens[] = {
	P("--white", BG),
	P("--bodyBackground", BG),
	P("--bodyText", TXT),
	P("--neutralPrimary", TXT),
	B("--neutralPrimaryAlt", TXT, BG, 0.15),
	P("--neutralSecondary", SUB),
	B("--neutralSecondaryAlt", SUB, BG, 0.15),
	B("--neutralTertiary", SUB, BG, 0.30),
	B("--neutralTertiaryAlt", BRD, BG, 0.20),
	P("--neutralQuaternary", BRD),
	B("--neutralQuaternaryAlt", BRD, BG, 0.40),
	P("--neutralLight", SURF),
	B("--neutralLighter", BG, SURF, 0.50),
	B("--neutralLighterAlt", BG, SURF, 0.25),
	P("--themePrimary", ACC),
	B("--themeSecondary", ACC, TXT, 0.15),
	B("--themeTertiary", ACC, BG, 0.40),
	K("--themeDarkAlt", ACC, 0.10),
	K("--themeDark", ACC, 0.20),
	K("--themeDarker", ACC, 0.35),
	B("--themeLight", ACC, BG, 0.60),
	B("--themeLighter", ACC, BG, 0.80),
	B("--themeLighterAlt", ACC, BG, 0.90),
	{ NULL, MIX_PART, BG, BG, 0.0 }
};

static void
blend(const BarWebmailRgb *a, const BarWebmailRgb *b, gdouble t,
      BarWebmailRgb *out)
{
	out->r = a->r + (b->r - a->r) * t;
	out->g = a->g + (b->g - a->g) * t;
	out->b = a->b + (b->b - a->b) * t;
}

/* Evaluate a recipe.  FALSE when a part it needs is not set. */
static gboolean
cook(const BarWebmailPalette *p, MixOp op, BarWebmailPart a,
     BarWebmailPart b, gdouble t, BarWebmailRgb *out)
{
	static const BarWebmailRgb black = { 0.0, 0.0, 0.0 };

	if (!p->set[a])
		return FALSE;
	switch (op) {
	case MIX_PART:
		*out = p->colors[a];
		return TRUE;
	case MIX_BLEND:
		if (!p->set[b])
			return FALSE;
		blend(&p->colors[a], &p->colors[b], t, out);
		return TRUE;
	case MIX_DARKEN:
		blend(&p->colors[a], &black, t, out);
		return TRUE;
	case MIX_ON:
		/* Whichever of the page's own text and ground reads
		   better on the accent: a pale accent gets dark text. */
		if (!p->set[BG] || !p->set[TXT])
			return FALSE;
		*out = (contrast(&p->colors[a], &p->colors[TXT]) >=
		        contrast(&p->colors[a], &p->colors[BG]))
		     ? p->colors[TXT] : p->colors[BG];
		return TRUE;
	default:
		return FALSE;
	}
}

/* One `prop: value[ !important];' line, if the recipe can be made. */
static void
decl(GString *body, const BarWebmailPalette *p, const gchar *prop,
     MixOp op, BarWebmailPart a, BarWebmailPart b, gdouble t,
     gboolean important)
{
	BarWebmailRgb c;

	if (!cook(p, op, a, b, t, &c))
		return;
	g_string_append_printf(body, "\t%s: ", prop);
	append_hex(body, &c);
	g_string_append(body, important ? " !important;\n" : ";\n");
}

/* `selector { body }' when the body has anything in it. */
static void
rule(GString *css, const gchar *selector, GString *body)
{
	if (body->len > 0)
		g_string_append_printf(css, "%s {\n%s}\n\n", selector,
		                       body->str);
	g_string_truncate(body, 0);
}

static void
tokens_block(GString *css, GString *body, const BarWebmailPalette *p,
             const gchar *selector, const Recipe *tokens)
{
	guint i;

	for (i = 0; tokens[i].name != NULL; i++)
		decl(body, p, tokens[i].name, tokens[i].op, tokens[i].a,
		     tokens[i].b, tokens[i].t, TRUE);
	rule(css, selector, body);
}

/* The colour-scheme line: lets form controls, scrollbars and any site
   that honours `prefers-color-scheme' pick the right half. */
static void
scheme_block(GString *css, const BarWebmailPalette *p)
{
	gint dark = bar_webmail_palette_dark(p);

	if (dark >= 0)
		g_string_append_printf(css,
			":root {\n\tcolor-scheme: %s !important;\n}\n\n",
			dark ? "dark" : "light");
}

static void
scrollbar_block(GString *css, GString *body, const BarWebmailPalette *p)
{
	BarWebmailRgb thumb, track;

	if (cook(p, MIX_PART, BRD, BRD, 0.0, &thumb) &&
	    cook(p, MIX_PART, BG, BG, 0.0, &track)) {
		g_string_append(body, "\tscrollbar-color: ");
		append_hex(body, &thumb);
		g_string_append_c(body, ' ');
		append_hex(body, &track);
		g_string_append(body, ";\n");
		rule(css, "*", body);
	}
	g_string_append(body, "\twidth: 10px;\n\theight: 10px;\n");
	decl(body, p, "background-color", MIX_PART, BG, BG, 0.0, FALSE);
	if (p->set[BG])
		rule(css, "::-webkit-scrollbar", body);
	else
		g_string_truncate(body, 0);
	decl(body, p, "background-color", MIX_PART, BRD, BRD, 0.0, FALSE);
	if (body->len > 0)
		g_string_append(body, "\tborder-radius: 8px;\n");
	rule(css, "::-webkit-scrollbar-thumb", body);
	decl(body, p, "background-color", MIX_PART, SUB, SUB, 0.0, FALSE);
	rule(css, "::-webkit-scrollbar-thumb:hover", body);
}

static void
selection_block(GString *css, GString *body, const BarWebmailPalette *p)
{
	decl(body, p, "background-color", MIX_PART, ACC, ACC, 0.0, FALSE);
	decl(body, p, "color", MIX_ON, ACC, ACC, 0.0, FALSE);
	rule(css, "::selection", body);
}

/* Outlook: Fluent tokens on every element that defines them. */
static void
outlook_css(GString *css, const BarWebmailPalette *p)
{
	/* Fluent defines its tokens on a generated class on the provider
	   element, not on :root, so :root alone loses.  Matching the
	   class by prefix keeps working as the hash suffix changes. */
	static const gchar *const token_selector =
		":root,\nbody,\n.fui-FluentProvider,\n"
		"[class*=\"fui-FluentProvider\"]";
	GString *body = g_string_new(NULL);

	scheme_block(css, p);
	tokens_block(css, body, p, token_selector, fluent_tokens);
	tokens_block(css, body, p, ":root,\nbody", fabric_tokens);

	decl(body, p, "background-color", MIX_PART, BG, BG, 0.0, TRUE);
	decl(body, p, "color", MIX_PART, TXT, TXT, 0.0, FALSE);
	rule(css, "html,\nbody", body);

	selection_block(css, body, p);
	scrollbar_block(css, body, p);
	g_string_free(body, TRUE);
}

/* Anything else: a small, forceful set of selectors.  Best effort by
   design -- a site with its own dark mode is better served by
   `prefers-color-scheme', which the colour-scheme line turns on. */
static void
generic_css(GString *css, const BarWebmailPalette *p)
{
	GString *body = g_string_new(NULL);

	scheme_block(css, p);

	decl(body, p, "background-color", MIX_PART, BG, BG, 0.0, TRUE);
	decl(body, p, "color", MIX_PART, TXT, TXT, 0.0, TRUE);
	rule(css, "html,\nbody", body);

	decl(body, p, "color", MIX_PART, ACC, ACC, 0.0, TRUE);
	rule(css, "a,\na:visited", body);

	decl(body, p, "background-color", MIX_PART, SURF, SURF, 0.0, TRUE);
	decl(body, p, "color", MIX_PART, TXT, TXT, 0.0, TRUE);
	decl(body, p, "border-color", MIX_PART, BRD, BRD, 0.0, TRUE);
	rule(css, "input,\ntextarea,\nselect,\nbutton", body);

	decl(body, p, "color", MIX_PART, SUB, SUB, 0.0, TRUE);
	rule(css, "::placeholder", body);

	selection_block(css, body, p);
	scrollbar_block(css, body, p);
	g_string_free(body, TRUE);
}

/**
 * bar_webmail_css:
 * @p: the resolved palette
 * @provider: which dialect to write
 * @extra_css: (nullable): the user's own CSS, appended last
 * @extra_name: (nullable): where @extra_css came from, for a comment
 *
 * The whole stylesheet the mail window loads.  Deterministic for a
 * given input (the golden test depends on it), and it writes only what
 * the palette can make: no part set and no extra CSS gives a file that
 * is a comment and nothing else.
 *
 * Returns: (transfer full): the CSS
 */
gchar *
bar_webmail_css(
	const BarWebmailPalette *p,
	BarWebmailProvider       provider,
	const gchar             *extra_css,
	const gchar             *extra_name
){
	GString *css = g_string_new(NULL);

	g_string_append_printf(css,
		"/* Generated by gowl's webmail widget for %s.\n"
		" * Rewritten on every launch and palette change: put your "
		"own rules in\n"
		" * the file named by the widget's `extra-css' setting. */\n\n",
		bar_webmail_provider_name(provider));

	if (provider == BAR_WEBMAIL_PROVIDER_OUTLOOK)
		outlook_css(css, p);
	else
		generic_css(css, p);

	if (extra_css != NULL && *extra_css != '\0') {
		g_autofree gchar *name = NULL;

		/* A path holding `*' `/' would end the comment early. */
		name = substitute((extra_name != NULL) ? extra_name : "extra",
		                  "*/", "* /", NULL);
		g_string_append_printf(css, "/* extra-css: %s */\n", name);
		g_string_append(css, extra_css);
		if (css->str[css->len - 1] != '\n')
			g_string_append_c(css, '\n');
	}
	return g_string_free(css, FALSE);
}

#undef P
#undef B
#undef K
#undef ON
#undef BG
#undef SURF
#undef TXT
#undef SUB
#undef ACC
#undef BRD

/* ================================================================
 * The gsurf configuration
 * ================================================================ */

/**
 * bar_webmail_yaml_quote:
 * @text: (nullable): any text
 *
 * A YAML double-quoted scalar holding exactly @text.  The URL and the
 * paths come from the user's configuration; unquoted, a URL with
 * `#' would lose its fragment to a comment and one with a newline
 * could add keys to the browser's configuration.
 *
 * Returns: (transfer full): the quoted scalar, quotes included
 */
gchar *
bar_webmail_yaml_quote(const gchar *text)
{
	g_autofree gchar *valid = NULL;
	GString *out;
	const guchar *p;

	valid = g_utf8_make_valid((text != NULL) ? text : "", -1);
	out = g_string_new("\"");
	for (p = (const guchar *)valid; *p != '\0'; p++) {
		if (*p == '"' || *p == '\\') {
			g_string_append_c(out, '\\');
			g_string_append_c(out, (gchar)*p);
		} else if (*p < 0x20 || *p == 0x7f) {
			g_string_append_printf(out, "\\x%02x", (guint)*p);
		} else {
			g_string_append_c(out, (gchar)*p);
		}
	}
	g_string_append_c(out, '"');
	return g_string_free(out, FALSE);
}

/*
 * gsurf modules the mail window turns OFF, with the reason.  gsurf
 * enables a module only when its section says so, but the C config
 * and a future default could turn one on, so they are stated.
 */
static const gchar *const modules_off[] = {
	"modal",          /* j/k/f would fight the mail app's own keys  */
	"find_bar",       /* bare `/' and n/N are mail shortcuts too    */
	"tabs",           /* one view: the kiosk window                 */
	"chromebar",
	"omnibar",
	"search_engines",
	"bookmarks",
	"history",        /* would log every subject line to disk       */
	"session",
	"toggles",        /* Ctrl+Shift+letter chords are Outlook's     */
	"homepage",
	"dark_mode",      /* the widget's own stylesheet sets the scheme */
	"adblock",
	"uri_params",
	"cert_manager",
	"useragent",
	"externalpipe",
	"playexternal",
	"mcp",
	"inspector",
	"spacesearch",
	"status_bar",
	"geolocation",
	NULL
};

/**
 * bar_webmail_gsurf_config:
 * @spec: what varies
 *
 * The YAML configuration the mail window runs with.  Generated, never
 * the user's own ~/.config/gsurf/config.yaml: the mail window is a
 * single-purpose kiosk with its own cookie jar, its own stylesheet and
 * a userscript, and none of that belongs in the browser the user
 * browses with.
 *
 * Returns: (transfer full): the configuration text
 */
gchar *
bar_webmail_gsurf_config(const BarWebmailGsurfSpec *spec)
{
	g_auto(GStrv) patterns = NULL;
	g_autofree gchar *q_url = NULL;
	g_autofree gchar *q_cache = NULL;
	g_autofree gchar *q_data = NULL;
	g_autofree gchar *q_jar = NULL;
	g_autofree gchar *q_dl = NULL;
	g_autofree gchar *q_css = NULL;
	g_autofree gchar *q_js = NULL;
	g_autofree gchar *jar = NULL;
	GString *y;
	guint i;

	jar   = g_build_filename(spec->profile_dir, "cookies.sqlite", NULL);
	q_url = bar_webmail_yaml_quote(spec->url);
	q_cache = bar_webmail_yaml_quote(spec->cache_dir);
	q_data  = bar_webmail_yaml_quote(spec->profile_dir);
	q_jar   = bar_webmail_yaml_quote(jar);
	q_dl    = bar_webmail_yaml_quote((spec->download_dir != NULL)
	                                 ? spec->download_dir
	                                 : "~/Downloads");
	q_css   = bar_webmail_yaml_quote(spec->css_path);
	q_js    = bar_webmail_yaml_quote(spec->js_path);
	patterns = bar_webmail_style_patterns(spec->url);

	y = g_string_new(NULL);
	g_string_append(y,
		"# Generated by gowl's webmail bar widget.  Rewritten on "
		"every launch;\n"
		"# change the widget's settings instead of this file.\n\n");

	g_string_append_printf(y,
		"browser:\n"
		"  homepage: %s\n"
		"  new_view_uri: %s\n"
		"  smooth_scroll: true\n\n", q_url, q_url);

	g_string_append(y,
		"window:\n"
		"  title: \"Web mail\"\n"
		"  geometry: 1280x860\n\n");

	g_string_append_printf(y,
		"storage:\n"
		"  cache_dir: %s\n"
		"  data_dir: %s\n"
		"  cookie_jar: %s\n"
		"  download_dir: %s\n"
		"  enable_disk_cache: true\n\n",
		q_cache, q_data, q_jar, q_dl);

	g_string_append(y,
		"webkit:\n"
		"  javascript: true\n"
		"  javascript_can_open_windows: false\n"
		"  media_stream: false\n"
		"  webrtc: false\n"
		"  developer_extras: false\n\n"
		"tls:\n"
		"  strict: true\n\n");

	/* The whole key table: with -c, these are the only bindings the
	   window has.  No bare letters -- a mail app owns those. */
	g_string_append(y,
		"keybinds:\n"
		"  \"Ctrl+r\": reload\n"
		"  \"Ctrl+Shift+r\": reload-nocache\n"
		"  \"Ctrl+plus\": zoom-in\n"
		"  \"Ctrl+minus\": zoom-out\n"
		"  \"Ctrl+0\": zoom-reset\n"
		"  \"Ctrl+q\": quit\n"
		"  \"F11\": toggle-fullscreen\n\n"
		"mousebinds:\n"
		"  \"Button8\": back\n"
		"  \"Button9\": forward\n\n");

	g_string_append(y, "modules:\n");

	/* The theme, limited to the mail host(s). */
	g_string_append(y,
		"  site_styles:\n"
		"    enabled: true\n"
		"    default: \"\"\n");
	if (patterns != NULL && patterns[0] != NULL) {
		g_string_append(y, "    rules:\n");
		for (i = 0; patterns[i] != NULL; i++) {
			g_autofree gchar *q_pat = NULL;

			q_pat = bar_webmail_yaml_quote(patterns[i]);
			g_string_append_printf(y,
				"      - match: %s\n"
				"        file: %s\n", q_pat, q_css);
		}
	} else {
		g_string_append(y, "    rules: []\n");
	}

	/* The unread counter.  Global: it is a no-op on pages with no
	   inbox, and the mail app may hop between its own hosts. */
	g_string_append_printf(y,
		"  userscripts:\n"
		"    enabled: true\n"
		"    global: %s\n"
		"    dir: \"\"\n"
		"    inject_time: end\n"
		"    rules: {}\n", q_js);

	g_string_append(y,
		"  notifications:\n"
		"    enabled: true\n"
		"    allow_web_notifications: allow\n"
		"    urgency: normal\n"
		"    notify_downloads: true\n");

	g_string_append_printf(y,
		"  downloads:\n"
		"    enabled: true\n"
		"    dir: %s\n"
		"    ask_destination: false\n"
		"    notify_complete: true\n", q_dl);

	/* Microsoft and Google renew a session from a hidden frame on
	   their login host, which is third-party from the mail page's
	   point of view.  Blocking it means signing in again every day
	   or so; this profile visits nothing but mail. */
	g_string_append_printf(y,
		"  cookie_policy:\n"
		"    enabled: true\n"
		"    policy: %s\n",
		spec->third_party_cookies ? "always" : "no-third-party");

	for (i = 0; modules_off[i] != NULL; i++)
		g_string_append_printf(y, "  %s:\n    enabled: false\n",
		                       modules_off[i]);

	g_string_append_printf(y, "\n# colour scheme: %s\n",
		(spec->dark > 0) ? "dark"
		                 : (spec->dark == 0) ? "light" : "site default");
	return g_string_free(y, FALSE);
}

/*
 * The userscript.  Outlook does not put the unread count in its title,
 * so this reads the Inbox folder's badge and writes "(N) " in front of
 * the page title, which the bar reads off the window.
 *
 * The folder is found by what is said to screen readers first
 * (`aria-label="Inbox, 3 unread"'), then by a badge-shaped child: the
 * class names Outlook ships are hashed and change between builds, the
 * accessibility text does not.  When no inbox is on the page (a login
 * screen, a site the selectors do not know) the title is left exactly
 * as the site wrote it -- Gmail and Proton already count for us.
 */
static const gchar unread_script[] =
	"/* Generated by gowl's webmail widget: puts the unread count in\n"
	"   the window title as \"(N) ...\" for the bar to read. */\n"
	"(function () {\n"
	"\t\"use strict\";\n"
	"\tif (window.top !== window.self || window.__gowlWebmailUnread)\n"
	"\t\treturn;\n"
	"\twindow.__gowlWebmailUnread = true;\n"
	"\n"
	"\tvar INBOX = /^\\s*(inbox|posteingang|bo[i\\u00ee]te de "
	"r[e\\u00e9]ception|bandeja de entrada|posta in arrivo|caixa de "
	"entrada|postvak in|inkorgen|indbakke|innboks|saapuneet|odebrane|"
	"doru[c\\u010d]en[a\\u00e1] po[s\\u0161]ta|\\u0432\\u0445\\u043e"
	"\\u0434\\u044f\\u0449\\u0438\\u0435|\\u53d7\\u4fe1\\u30c8\\u30ec"
	"\\u30a4|\\u6536\\u4ef6\\u7bb1|\\ubc1b\\uc740\\ud3b8\\uc9c0\\ud568)"
	"(?=$|[\\s,.:;(0-9])/i;\n"
	"\tvar UNREAD = /(\\d[\\d.,\\u00a0\\u202f]*)\\s*\\+?\\s*(unread|"
	"ungelesen|non lus?|no le[i\\u00ed]dos?|non lett[ie]|n[a\\u00e3]o "
	"lid[ao]s?|ongelezen|ol[a\\u00e4]sta|\\u043d\\u0435\\u043f\\u0440"
	"\\u043e\\u0447\\u0438\\u0442\\u0430\\u043d\\u043d)/i;\n"
	"\tvar BADGE = /^\\s*\\d{1,6}\\+?\\s*$/;\n"
	"\tvar PREFIX = /^\\s*\\(\\d[\\d.,]*\\+?\\)\\s*/;\n"
	"\n"
	"\tfunction digits(s) {\n"
	"\t\tvar n = parseInt(String(s).replace(/[^\\d]/g, \"\"), 10);\n"
	"\t\treturn isNaN(n) ? null : n;\n"
	"\t}\n"
	"\n"
	"\tfunction label_of(el) {\n"
	"\t\treturn (el.getAttribute(\"aria-label\") || \"\") + \" \" +\n"
	"\t\t       (el.getAttribute(\"title\") || \"\");\n"
	"\t}\n"
	"\n"
	"\t/* The unread number of one folder node; 0 when it has none. */\n"
	"\tfunction from_node(el) {\n"
	"\t\tvar m = UNREAD.exec(label_of(el)), kids, i;\n"
	"\n"
	"\t\tif (m)\n"
	"\t\t\treturn digits(m[1]);\n"
	"\t\tkids = el.querySelectorAll(\"span, div\");\n"
	"\t\tfor (i = 0; i < kids.length && i < 64; i++) {\n"
	"\t\t\tif (kids[i].children.length === 0 &&\n"
	"\t\t\t    BADGE.test(kids[i].textContent || \"\"))\n"
	"\t\t\t\treturn digits(kids[i].textContent);\n"
	"\t\t}\n"
	"\t\treturn 0;\n"
	"\t}\n"
	"\n"
	"\tfunction count() {\n"
	"\t\tvar el, nodes, i, name;\n"
	"\n"
	"\t\tel = document.querySelector('[data-folder-name=\"inbox\" i]');\n"
	"\t\tif (el)\n"
	"\t\t\treturn from_node(el);\n"
	"\t\tnodes = document.querySelectorAll('[role=\"treeitem\"], '\n"
	"\t\t\t+ '[role=\"navigation\"] a[aria-label], nav a[title]');\n"
	"\t\tfor (i = 0; i < nodes.length && i < 400; i++) {\n"
	"\t\t\tname = nodes[i].getAttribute(\"aria-label\") ||\n"
	"\t\t\t       nodes[i].getAttribute(\"title\") ||\n"
	"\t\t\t       nodes[i].textContent || \"\";\n"
	"\t\t\tif (INBOX.test(name))\n"
	"\t\t\t\treturn from_node(nodes[i]);\n"
	"\t\t}\n"
	"\t\treturn null;\n"
	"\t}\n"
	"\n"
	"\tfunction apply() {\n"
	"\t\tvar n = count(), title, bare, want;\n"
	"\n"
	"\t\tif (n === null)\n"
	"\t\t\treturn;\n"
	"\t\ttitle = document.title || \"\";\n"
	"\t\tbare = title.replace(PREFIX, \"\");\n"
	"\t\twant = (n > 0) ? \"(\" + n + \") \" + bare : bare;\n"
	"\t\tif (want !== title)\n"
	"\t\t\tdocument.title = want;\n"
	"\t}\n"
	"\n"
	"\tapply();\n"
	"\twindow.setInterval(apply, 1500);\n"
	"})();\n";

/**
 * bar_webmail_unread_script:
 *
 * Returns: (transfer none): the userscript source
 */
const gchar *
bar_webmail_unread_script(void)
{
	return unread_script;
}

/* ================================================================
 * Decisions
 * ================================================================ */

/**
 * bar_webmail_decide_toggle:
 * @state: what the window is doing
 * @bring_here: `show-on: here' -- move a window from another tag to the
 *   user rather than jump the user to it
 *
 * The left click, which is a toggle: in front means hide it, anywhere
 * else means show it, and nothing at all means start it.  A window
 * that is starting is left alone: launching again would give two.
 *
 * Returns: what to do
 */
BarWebmailAction
bar_webmail_decide_toggle(BarWebmailWindow state, gboolean bring_here)
{
	switch (state) {
	case BAR_WEBMAIL_WINDOW_NONE:      return BAR_WEBMAIL_ACTION_LAUNCH;
	case BAR_WEBMAIL_WINDOW_STARTING:  return BAR_WEBMAIL_ACTION_WAIT;
	case BAR_WEBMAIL_WINDOW_FOCUSED:   return BAR_WEBMAIL_ACTION_HIDE;
	case BAR_WEBMAIL_WINDOW_VISIBLE:   return BAR_WEBMAIL_ACTION_FOCUS;
	case BAR_WEBMAIL_WINDOW_ELSEWHERE:
		return bring_here ? BAR_WEBMAIL_ACTION_BRING
		                  : BAR_WEBMAIL_ACTION_JUMP;
	case BAR_WEBMAIL_WINDOW_MINIMIZED: return BAR_WEBMAIL_ACTION_RESTORE;
	default:                           return BAR_WEBMAIL_ACTION_NONE;
	}
}

/**
 * bar_webmail_decide_show:
 * @state: what the window is doing
 * @bring_here: as for bar_webmail_decide_toggle()
 *
 * The right click and the panel's Open: always towards the window,
 * never away.  A focused window stays focused.
 *
 * Returns: what to do
 */
BarWebmailAction
bar_webmail_decide_show(BarWebmailWindow state, gboolean bring_here)
{
	if (state == BAR_WEBMAIL_WINDOW_FOCUSED)
		return BAR_WEBMAIL_ACTION_FOCUS;
	return bar_webmail_decide_toggle(state, bring_here);
}

/**
 * bar_webmail_look:
 * @running: whether the browser process is alive
 * @unread: the unread count, -1 when unknown
 *
 * Returns: which colour setting the bar label takes
 */
BarWebmailLook
bar_webmail_look(gboolean running, gint unread)
{
	if (!running)
		return BAR_WEBMAIL_LOOK_OFF;
	return (unread > 0) ? BAR_WEBMAIL_LOOK_UNREAD : BAR_WEBMAIL_LOOK_ON;
}

/**
 * bar_webmail_label:
 * @unread: the unread count, -1 when unknown
 * @plus: whether the count was capped
 * @show_count: the `show-count' setting
 *
 * The text after the mail glyph: a dot and the count when there is
 * unread mail, the dot alone with `show-count: false', nothing
 * otherwise.  Four digits is where a bar item stops being an icon, so
 * 1000 and up read "999+".
 *
 * Returns: (transfer full) (nullable): the label, or %NULL for none
 */
gchar *
bar_webmail_label(gint unread, gboolean plus, gboolean show_count)
{
	if (unread <= 0)
		return NULL;
	if (!show_count)
		return g_strdup("\xe2\x97\x8f");
	if (unread > 999)
		return g_strdup("\xe2\x97\x8f 999+");
	return g_strdup_printf("\xe2\x97\x8f %d%s", unread, plus ? "+" : "");
}

/**
 * bar_webmail_confirm_armed:
 * @armed_at: monotonic time of the first click, 0 when not armed
 * @now: monotonic time now
 *
 * Whether a destructive button's first click is still live.  Five
 * seconds: long enough to read "Click again to quit", short enough
 * that coming back to the panel later does not find it loaded.
 *
 * Returns: %TRUE when the next click should act
 */
gboolean
bar_webmail_confirm_armed(gint64 armed_at, gint64 now)
{
	return armed_at > 0 && now >= armed_at &&
	       now - armed_at < 5 * G_TIME_SPAN_SECOND;
}
