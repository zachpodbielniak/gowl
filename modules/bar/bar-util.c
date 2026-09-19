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

#include "bar-util.h"

#include <string.h>

/**
 * SECTION:bar-util
 * @title: Bar plugin helpers
 * @short_description: pure functions shared by the shipped plugins
 */

gboolean
bar_shell_needs_sh(const gchar *cmdline)
{
	const gchar *p;

	if (cmdline == NULL)
		return FALSE;

	/* Anything g_shell_parse_argv() would either pass through as a
	   literal or reject outright.  Quotes are NOT in this set: the
	   argv parser handles them, and a quoted argument is the common
	   case for a command with a space in a path. */
	for (p = cmdline; *p != '\0'; p++) {
		switch (*p) {
		case '$':
		case '|':
		case '&':
		case ';':
		case '<':
		case '>':
		case '(':
		case ')':
		case '`':
		case '*':
		case '?':
		case '[':
		case '{':
		case '\n':
			return TRUE;
		default:
			break;
		}
	}
	return FALSE;
}

void
bar_strip_ansi(gchar *text)
{
	gchar *src, *dst;

	if (text == NULL)
		return;

	src = dst = text;
	while (*src != '\0') {
		if (*src != '\033') {
			*dst++ = *src++;
			continue;
		}

		src++;
		if (*src == '[') {
			/* CSI: parameter and intermediate bytes, then one
			   final byte in 0x40--0x7E. */
			src++;
			while (*src != '\0' && (*src < 0x40 || *src > 0x7E))
				src++;
			if (*src != '\0')
				src++;
		} else if (*src == ']') {
			/* OSC: up to BEL or ESC \. */
			src++;
			while (*src != '\0' && *src != '\007' &&
			       !(*src == '\033' && src[1] == '\\'))
				src++;
			if (*src == '\007')
				src++;
			else if (*src == '\033')
				src += 2;
		} else if (*src != '\0') {
			/* A two-byte escape (ESC c, ESC 7 ...). */
			src++;
		}
	}
	*dst = '\0';
}

gboolean
bar_strftime_has_seconds(const gchar *format)
{
	const gchar *p;

	if (format == NULL)
		return FALSE;

	for (p = format; *p != '\0'; p++) {
		if (*p != '%')
			continue;
		p++;
		/* Flags, a width, and the E/O alternative markers. */
		while (*p == '-' || *p == '_' || *p == '0' || *p == '^' ||
		       *p == '#' || *p == 'E' || *p == 'O' ||
		       (*p >= '1' && *p <= '9'))
			p++;
		switch (*p) {
		case 'S':   /* seconds */
		case 'T':   /* %H:%M:%S */
		case 'r':   /* %I:%M:%S %p */
		case 'X':   /* the locale's time, with seconds */
		case 'c':   /* the locale's date and time */
		case 's':   /* the epoch, which moves every second */
		case '+':   /* date(1) format */
			return TRUE;
		case '\0':
			return FALSE;
		default:
			break;
		}
	}
	return FALSE;
}

gchar *
bar_git_head_path(const gchar *cwd)
{
	g_autofree gchar *dir = NULL;

	if (cwd == NULL || cwd[0] != '/')
		return NULL;

	dir = g_strdup(cwd);
	for (;;) {
		g_autofree gchar *dotgit = NULL;
		gchar *slash;

		dotgit = g_build_filename(dir, ".git", NULL);

		if (g_file_test(dotgit, G_FILE_TEST_IS_DIR))
			return g_build_filename(dotgit, "HEAD", NULL);

		if (g_file_test(dotgit, G_FILE_TEST_IS_REGULAR)) {
			g_autofree gchar *body = NULL;
			gchar *gitdir;

			/* A worktree or a submodule: `gitdir: <path>'. */
			if (!g_file_get_contents(dotgit, &body, NULL, NULL))
				return NULL;
			g_strstrip(body);
			if (!g_str_has_prefix(body, "gitdir:"))
				return NULL;
			gitdir = body + strlen("gitdir:");
			while (*gitdir == ' ' || *gitdir == '\t')
				gitdir++;
			if (gitdir[0] == '\0')
				return NULL;
			if (gitdir[0] == '/')
				return g_build_filename(gitdir, "HEAD", NULL);
			return g_build_filename(dir, gitdir, "HEAD", NULL);
		}

		slash = strrchr(dir, '/');
		if (slash == NULL || slash == dir)
			return NULL;
		*slash = '\0';
	}
}

gchar *
bar_git_ref_label(const gchar *head)
{
	g_autofree gchar *copy = NULL;

	if (head == NULL)
		return g_strdup("");

	copy = g_strdup(head);
	g_strstrip(copy);

	if (g_str_has_prefix(copy, "ref: refs/heads/"))
		return g_strdup(copy + strlen("ref: refs/heads/"));
	if (g_str_has_prefix(copy, "ref: "))
		return g_strdup(copy + strlen("ref: "));

	/* A detached HEAD is a commit id.  Forty hex digits is a label
	   that pushes everything else off the bar; eight is what
	   `git log --oneline' shows and is enough to find it again. */
	if (strlen(copy) >= 40) {
		const gchar *p;
		gboolean hex = TRUE;

		for (p = copy; *p != '\0' && hex; p++)
			hex = g_ascii_isxdigit(*p);
		if (hex)
			return g_strndup(copy, 8);
	}
	return g_steal_pointer(&copy);
}

gint
bar_iso_week_of_row(GDateTime *first)
{
	gint dow;

	g_return_val_if_fail(first != NULL, 1);

	/* 1 = Monday ... 7 = Sunday.  A Sunday-first row's Monday is the
	   next day; any other first day is on or after its own Monday and
	   already reads the right week. */
	dow = g_date_time_get_day_of_week(first);
	if (dow == 7) {
		GDateTime *monday = g_date_time_add_days(first, 1);
		gint week = g_date_time_get_week_of_year(monday);

		g_date_time_unref(monday);
		return week;
	}
	return g_date_time_get_week_of_year(first);
}

gchar *
bar_elisp_quote(const gchar *text)
{
	GString *out;
	const gchar *p;

	out = g_string_new(NULL);
	for (p = (text != NULL) ? text : ""; *p != '\0'; p++) {
		if (*p == '\\' || *p == '"')
			g_string_append_c(out, '\\');
		g_string_append_c(out, *p);
	}
	return g_string_free(out, FALSE);
}
