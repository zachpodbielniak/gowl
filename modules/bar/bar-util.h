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

#ifndef BAR_UTIL_H
#define BAR_UTIL_H

#include <glib.h>

G_BEGIN_DECLS

/*
 * Small pure functions the shipped plugins share.
 *
 * Nothing here touches the compositor, a file the caller did not name,
 * or a subprocess, so every one of them can be exercised from
 * tests/test-bar-util.c against the exact input that went wrong.
 */

/**
 * bar_shell_needs_sh:
 * @cmdline: a command line from a setting or a plugin
 *
 * Whether @cmdline uses anything only a shell can interpret: a pipe, a
 * redirection, `&&', a `$(...)' substitution, a variable, a glob or a
 * `;'.  A line with none of those is run as an argv directly; one with
 * any of them is handed to `sh -c', because g_shell_parse_argv() only
 * splits words and quotes -- it hands `$(slurp)' to the program as a
 * literal argument and `&&' as another.
 *
 * Returns: %TRUE when @cmdline must go through a shell
 */
gboolean bar_shell_needs_sh (const gchar *cmdline);

/**
 * bar_strip_ansi:
 * @text: (inout): a string, rewritten in place
 *
 * Removes every ANSI escape sequence: CSI sequences of any kind (not
 * only the `m' colour ones -- a script that clears to end of line
 * emits `ESC [ K', and treating that as "skip to the next m" swallowed
 * everything up to the next letter m in the text), OSC sequences up to
 * their terminator, and lone two-byte escapes.
 */
void bar_strip_ansi (gchar *text);

/**
 * bar_strftime_has_seconds:
 * @format: a strftime() format
 *
 * Whether @format shows seconds, so a clock can tick once a second for
 * it and once every few seconds otherwise.  Looks through `%%' and the
 * `-', `_', `0', `^', `#', `E' and `O' modifiers glibc accepts.
 *
 * Returns: %TRUE when the format prints seconds
 */
gboolean bar_strftime_has_seconds (const gchar *format);

/**
 * bar_git_head_path:
 * @cwd: a directory inside a working tree, or not
 *
 * Walks up from @cwd to the nearest git working tree and returns the
 * path of its HEAD.  `.git' may be a directory (an ordinary checkout)
 * or a file reading `gitdir: <path>' (a worktree or a submodule); the
 * second form used to be missed entirely, so every worktree showed no
 * branch at all.  A relative gitdir is resolved against the directory
 * holding the `.git' file.
 *
 * Returns: (transfer full) (nullable): the path to HEAD, or %NULL when
 *   @cwd is not inside a working tree
 */
gchar *bar_git_head_path (const gchar *cwd);

/**
 * bar_git_ref_label:
 * @head: the contents of a HEAD file
 *
 * The branch name out of `ref: refs/heads/<name>', or the first eight
 * characters of a detached commit id -- forty of them do not fit in a
 * bar, and eight is what `git log --oneline' shows.
 *
 * Returns: (transfer full): the label
 */
gchar *bar_git_ref_label (const gchar *head);

/**
 * bar_iso_week_of_row:
 * @first: the first day of a calendar row
 *
 * The ISO week number to print beside a calendar row.  ISO weeks start
 * on Monday, and a calendar drawn with Sunday first shows six of every
 * seven days of the row belonging to the week AFTER the Sunday's -- so
 * the number is taken from the Monday in the row, not the first cell.
 * A row starting on Monday reads its own week.
 *
 * Returns: the week number, 1--53
 */
gint bar_iso_week_of_row (GDateTime *first);

/**
 * bar_elisp_quote:
 * @text: a string to put inside an Elisp string literal
 *
 * Escapes `\' and `"', which are the only two characters an Elisp
 * string literal needs escaped.  A capture path with a quote in it
 * used to break the `emacsctl eval' line at that quote.
 *
 * Returns: (transfer full): the escaped text, without the quotes
 */
gchar *bar_elisp_quote (const gchar *text);

G_END_DECLS

#endif /* BAR_UTIL_H */
