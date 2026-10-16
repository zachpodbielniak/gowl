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
 * The webmail widget's pure half: everything that can be decided from
 * plain values -- a window title, a URL, a palette -- with no plugin,
 * theme, compositor or subprocess in sight.
 *
 * Kept apart so the test can include the .c directly and pin down the
 * behaviour that matters (what counts as an unread count, what CSS a
 * palette becomes, what a click does in each window state) without a
 * compositor to make real windows in.
 */

#ifndef BAR_WEBMAIL_MODEL_H
#define BAR_WEBMAIL_MODEL_H

#include <glib.h>
#include <sys/types.h>

G_BEGIN_DECLS

/**
 * BarWebmailProvider:
 * @BAR_WEBMAIL_PROVIDER_GENERIC: any other web mail (Roundcube, SOGo, ...)
 * @BAR_WEBMAIL_PROVIDER_OUTLOOK: Outlook on the web, work or personal
 * @BAR_WEBMAIL_PROVIDER_GMAIL: Gmail
 * @BAR_WEBMAIL_PROVIDER_FASTMAIL: Fastmail
 * @BAR_WEBMAIL_PROVIDER_PROTON: Proton Mail
 *
 * Which site the configured URL points at.  It decides the compose
 * deep link, the hosts the generated stylesheet is scoped to, and
 * whether the stylesheet speaks Fluent UI or the generic dialect.
 */
typedef enum {
	BAR_WEBMAIL_PROVIDER_GENERIC = 0,
	BAR_WEBMAIL_PROVIDER_OUTLOOK,
	BAR_WEBMAIL_PROVIDER_GMAIL,
	BAR_WEBMAIL_PROVIDER_FASTMAIL,
	BAR_WEBMAIL_PROVIDER_PROTON
} BarWebmailProvider;

/**
 * BarWebmailRgb:
 * @r: red, 0..1
 * @g: green, 0..1
 * @b: blue, 0..1
 *
 * One opaque colour.
 */
typedef struct {
	gdouble r;
	gdouble g;
	gdouble b;
} BarWebmailRgb;

/**
 * BarWebmailPart:
 * @BAR_WEBMAIL_PART_BACKGROUND: the page ground
 * @BAR_WEBMAIL_PART_SURFACE: raised rows, hovered and selected items
 * @BAR_WEBMAIL_PART_TEXT: primary text
 * @BAR_WEBMAIL_PART_SUBTEXT: secondary text
 * @BAR_WEBMAIL_PART_ACCENT: the brand colour: buttons, links, selection
 * @BAR_WEBMAIL_PART_BORDER: hairlines and scrollbar thumbs
 * @BAR_WEBMAIL_N_PARTS: sentinel
 *
 * The six colours a user can set one by one (`color-<part>').  Every
 * CSS value the widget writes is one of these or a mix of them.
 */
typedef enum {
	BAR_WEBMAIL_PART_BACKGROUND = 0,
	BAR_WEBMAIL_PART_SURFACE,
	BAR_WEBMAIL_PART_TEXT,
	BAR_WEBMAIL_PART_SUBTEXT,
	BAR_WEBMAIL_PART_ACCENT,
	BAR_WEBMAIL_PART_BORDER,
	BAR_WEBMAIL_N_PARTS
} BarWebmailPart;

/**
 * BarWebmailPalette:
 * @colors: one colour per #BarWebmailPart
 * @set: whether that part has a colour at all
 *
 * The resolved colours.  A part that is not @set is simply not
 * written: with `follow-theme: false' and two overrides the
 * stylesheet themes exactly those two things and leaves the site's
 * own colours alone everywhere else.
 */
typedef struct {
	BarWebmailRgb colors[BAR_WEBMAIL_N_PARTS];
	gboolean      set[BAR_WEBMAIL_N_PARTS];
} BarWebmailPalette;

/**
 * BarWebmailWindow:
 * @BAR_WEBMAIL_WINDOW_NONE: no browser process
 * @BAR_WEBMAIL_WINDOW_STARTING: the process runs but has not mapped
 * @BAR_WEBMAIL_WINDOW_FOCUSED: mapped and holding keyboard focus
 * @BAR_WEBMAIL_WINDOW_VISIBLE: on a tag its output is viewing, unfocused
 * @BAR_WEBMAIL_WINDOW_ELSEWHERE: mapped on a tag nobody is viewing
 * @BAR_WEBMAIL_WINDOW_MINIMIZED: hidden by this widget (or anything else)
 *
 * What the mail window is doing, as far as a click cares.
 */
typedef enum {
	BAR_WEBMAIL_WINDOW_NONE = 0,
	BAR_WEBMAIL_WINDOW_STARTING,
	BAR_WEBMAIL_WINDOW_FOCUSED,
	BAR_WEBMAIL_WINDOW_VISIBLE,
	BAR_WEBMAIL_WINDOW_ELSEWHERE,
	BAR_WEBMAIL_WINDOW_MINIMIZED
} BarWebmailWindow;

/**
 * BarWebmailAction:
 * @BAR_WEBMAIL_ACTION_NONE: nothing to do
 * @BAR_WEBMAIL_ACTION_LAUNCH: start the browser
 * @BAR_WEBMAIL_ACTION_WAIT: it is starting; a second launch would be a
 *   second window
 * @BAR_WEBMAIL_ACTION_HIDE: minimize it
 * @BAR_WEBMAIL_ACTION_RESTORE: unminimize it where the user is
 * @BAR_WEBMAIL_ACTION_FOCUS: focus it where it already is
 * @BAR_WEBMAIL_ACTION_JUMP: switch to the tag it is on and focus it
 * @BAR_WEBMAIL_ACTION_BRING: move it to where the user is and focus it
 *
 * What a click turns into.
 */
typedef enum {
	BAR_WEBMAIL_ACTION_NONE = 0,
	BAR_WEBMAIL_ACTION_LAUNCH,
	BAR_WEBMAIL_ACTION_WAIT,
	BAR_WEBMAIL_ACTION_HIDE,
	BAR_WEBMAIL_ACTION_RESTORE,
	BAR_WEBMAIL_ACTION_FOCUS,
	BAR_WEBMAIL_ACTION_JUMP,
	BAR_WEBMAIL_ACTION_BRING
} BarWebmailAction;

/**
 * BarWebmailLook:
 * @BAR_WEBMAIL_LOOK_OFF: not running: dimmed
 * @BAR_WEBMAIL_LOOK_ON: running, nothing unread
 * @BAR_WEBMAIL_LOOK_UNREAD: running with unread mail
 *
 * Which of the three `color-*' settings the bar label uses.
 */
typedef enum {
	BAR_WEBMAIL_LOOK_OFF = 0,
	BAR_WEBMAIL_LOOK_ON,
	BAR_WEBMAIL_LOOK_UNREAD
} BarWebmailLook;

/**
 * BarWebmailGsurfSpec:
 * @url: the mail URL, the window's homepage
 * @profile_dir: where cookies and site data persist
 * @cache_dir: the HTTP cache
 * @css_path: the generated stylesheet
 * @js_path: the generated unread-count userscript
 * @download_dir: (nullable): where attachments go; %NULL for ~/Downloads
 * @dark: 1 dark, 0 light, -1 unknown (no background colour)
 * @third_party_cookies: whether sign-in iframes may keep cookies
 *
 * Everything that varies in the generated gsurf configuration.
 */
typedef struct {
	const gchar *url;
	const gchar *profile_dir;
	const gchar *cache_dir;
	const gchar *css_path;
	const gchar *js_path;
	const gchar *download_dir;
	gint         dark;
	gboolean     third_party_cookies;
} BarWebmailGsurfSpec;

/* --- Titles ------------------------------------------------------- */

gint bar_webmail_parse_unread (const gchar *title,
                               gboolean    *plus);

/* --- URLs --------------------------------------------------------- */

gchar             *bar_webmail_url_host       (const gchar *url);
BarWebmailProvider bar_webmail_provider       (const gchar *url);
const gchar       *bar_webmail_provider_name  (BarWebmailProvider provider);
gchar             *bar_webmail_compose_url    (const gchar *url);
gchar            **bar_webmail_style_patterns (const gchar *url);

/* --- Commands ----------------------------------------------------- */

gchar  **bar_webmail_build_argv (const gchar  *template_,
                                 const gchar  *config_path,
                                 const gchar  *url,
                                 GError      **error);
gboolean bar_webmail_cmdline_has_arg (const gchar *cmdline,
                                      gsize        len,
                                      const gchar *needle);
pid_t    bar_webmail_stat_ppid       (const gchar *stat_text);

/* --- Colour ------------------------------------------------------- */

gboolean bar_webmail_parse_hex          (const gchar         *spec,
                                         BarWebmailRgb       *out);
gdouble  bar_webmail_luminance          (const BarWebmailRgb *c);
gint     bar_webmail_palette_dark       (const BarWebmailPalette *p);
gchar   *bar_webmail_palette_signature  (const BarWebmailPalette *p);
const gchar *bar_webmail_part_name      (BarWebmailPart part);

/* --- Generated files ---------------------------------------------- */

gchar       *bar_webmail_css            (const BarWebmailPalette *p,
                                         BarWebmailProvider       provider,
                                         const gchar             *extra_css,
                                         const gchar             *extra_name);
gchar       *bar_webmail_yaml_quote     (const gchar *text);
gchar       *bar_webmail_gsurf_config   (const BarWebmailGsurfSpec *spec);
const gchar *bar_webmail_unread_script  (void);

/* --- Decisions ---------------------------------------------------- */

BarWebmailAction bar_webmail_decide_toggle (BarWebmailWindow state,
                                            gboolean         bring_here);
BarWebmailAction bar_webmail_decide_show   (BarWebmailWindow state,
                                            gboolean         bring_here);
BarWebmailLook   bar_webmail_look          (gboolean running,
                                            gint     unread);
gchar           *bar_webmail_label         (gint     unread,
                                            gboolean plus,
                                            gboolean show_count);
gboolean         bar_webmail_confirm_armed (gint64 armed_at,
                                            gint64 now);

G_END_DECLS

#endif /* BAR_WEBMAIL_MODEL_H */
