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
 * The `webmail' bar widget (modules/bar/bar-plugins-webmail.c and its
 * pure half, bar-webmail-model.c), offline.
 *
 * The pure half is included directly and tested value by value: what
 * counts as an unread count, what CSS a palette becomes, what gsurf
 * configuration is written, how the command line is built, and what a
 * click does in each window state.
 *
 * The widget itself runs in the bar-widget harness with a stand-in
 * `gsurf' -- a script that writes its argv down and stays alive until
 * it is signalled.  No real browser and no compositor: a harness
 * cannot make Wayland clients, which is why the window decisions are
 * a pure function (bar_webmail_decide_toggle) tested here as a table.
 *
 * Golden files live in tests/fixtures/bar/webmail/.  To regenerate
 * them after an intended change, run with GOWL_WEBMAIL_REGOLD=1 and
 * read the diff before committing it.
 */

#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <sys/stat.h>
#include <linux/input-event-codes.h>

#define BAR_HARNESS_MODULE GOWL_TEST_BAR_MODULE
#include "bar-widget-harness.h"

#include "../modules/bar/bar-webmail-model.c"

#ifndef GOWL_TEST_FIXTURES
#define GOWL_TEST_FIXTURES "tests/fixtures/bar"
#endif

/* wlroots' WLR_MODIFIER_SHIFT, without pulling wlroots into a test. */
#define WLR_TEST_MODIFIER_SHIFT (1u << 0)

/* How many launches run-in-background allows a browser that exits at
   once: the widget's WM_MAX_QUICK_DEATHS. */
#define WM_TEST_MAX_LAUNCHES (3u)

/* ----------------------------------------------------------------
 * Helpers
 * ---------------------------------------------------------------- */

static void
palette_set(BarWebmailPalette *p, BarWebmailPart part, const gchar *hex)
{
	g_assert_true(bar_webmail_parse_hex(hex, &p->colors[part]));
	p->set[part] = TRUE;
}

/* Catppuccin Mocha, as the bar's default theme resolves it. */
static void
palette_mocha(BarWebmailPalette *p)
{
	memset(p, 0, sizeof(*p));
	palette_set(p, BAR_WEBMAIL_PART_BACKGROUND, "#1e1e2e");
	palette_set(p, BAR_WEBMAIL_PART_SURFACE,    "#313244");
	palette_set(p, BAR_WEBMAIL_PART_TEXT,       "#cdd6f4");
	palette_set(p, BAR_WEBMAIL_PART_SUBTEXT,    "#a6adc8");
	palette_set(p, BAR_WEBMAIL_PART_ACCENT,     "#89b4fa");
	palette_set(p, BAR_WEBMAIL_PART_BORDER,     "#6c7086");
}

/* Catppuccin Latte. */
static void
palette_latte(BarWebmailPalette *p)
{
	memset(p, 0, sizeof(*p));
	palette_set(p, BAR_WEBMAIL_PART_BACKGROUND, "#eff1f5");
	palette_set(p, BAR_WEBMAIL_PART_SURFACE,    "#ccd0da");
	palette_set(p, BAR_WEBMAIL_PART_TEXT,       "#4c4f69");
	palette_set(p, BAR_WEBMAIL_PART_SUBTEXT,    "#6c6f85");
	palette_set(p, BAR_WEBMAIL_PART_ACCENT,     "#1e66f5");
	palette_set(p, BAR_WEBMAIL_PART_BORDER,     "#9ca0b0");
}

/*
 * Compare @actual with a golden fixture, or write the fixture when
 * GOWL_WEBMAIL_REGOLD is set.
 */
static void
assert_golden(const gchar *name, const gchar *actual)
{
	g_autofree gchar *path = NULL;
	g_autofree gchar *expected = NULL;

	path = g_build_filename(GOWL_TEST_FIXTURES, "webmail", name, NULL);
	if (g_getenv("GOWL_WEBMAIL_REGOLD") != NULL) {
		g_assert_true(g_file_set_contents(path, actual, -1, NULL));
		return;
	}
	g_assert_true(g_file_get_contents(path, &expected, NULL, NULL));
	g_assert_cmpstr(actual, ==, expected);
}

/* Read a whole file, failing the test when it is not there. */
static gchar *
slurp(const gchar *path)
{
	gchar *text = NULL;

	g_assert_true(g_file_get_contents(path, &text, NULL, NULL));
	return text;
}

/* Wait (briefly, bounded) for a file to exist with at least @lines
   newline-terminated lines. */
static gboolean
wait_for_lines(const gchar *path, guint lines)
{
	guint tries;

	for (tries = 0; tries < 100; tries++) {
		g_autofree gchar *text = NULL;
		guint n = 0;
		const gchar *p;

		if (g_file_get_contents(path, &text, NULL, NULL)) {
			for (p = text; *p != '\0'; p++)
				if (*p == '\n')
					n++;
			if (n >= lines)
				return TRUE;
		}
		g_usleep(20 * 1000);
	}
	return FALSE;
}

static guint
count_lines_with(const gchar *path, const gchar *needle)
{
	g_autofree gchar *text = NULL;
	g_auto(GStrv) lines = NULL;
	guint i, n = 0;

	if (!g_file_get_contents(path, &text, NULL, NULL))
		return 0;
	lines = g_strsplit(text, "\n", -1);
	for (i = 0; lines[i] != NULL; i++)
		if (g_strcmp0(lines[i], needle) == 0)
			n++;
	return n;
}

/* The PID the widget recorded, from its pid file. */
static GPid
read_pid(const gchar *run_dir)
{
	g_autofree gchar *path = g_build_filename(run_dir, "pid", NULL);
	g_autofree gchar *text = NULL;

	if (!g_file_get_contents(path, &text, NULL, NULL))
		return 0;
	return (GPid)g_ascii_strtoll(text, NULL, 10);
}

static gboolean
wait_dead(GPid pid)
{
	guint tries;

	for (tries = 0; tries < 100; tries++) {
		g_autofree gchar *stat = NULL;
		g_autofree gchar *path = g_strdup_printf("/proc/%d/stat",
		                                         (gint)pid);

		if (!g_file_get_contents(path, &stat, NULL, NULL))
			return TRUE;
		if (strstr(stat, ") Z") != NULL)
			return TRUE;
		g_usleep(20 * 1000);
	}
	return FALSE;
}

/*
 * A widget in the harness with its runtime and profile directories in
 * a scratch dir of their own (not the harness's, which a second
 * harness in the same test could not share), and a stand-in browser
 * that appends its argv to `argv.log', one argument per line with a
 * `--' line after each launch, then waits to be signalled.
 */
typedef struct {
	BarHarness h;
	gchar     *dir;
	gchar     *run_dir;
	gchar     *profile_dir;
	gchar     *argv_log;
	gchar     *fake;
} Rig;

static gboolean
rig_up(Rig *r, const gchar *fake_body)
{
	g_autofree gchar *cmd = NULL;
	g_autofree gchar *body = NULL;

	memset(r, 0, sizeof(*r));
	if (!bar_harness_up(&r->h, "bar_register_webmail_plugins", "webmail"))
		return FALSE;
	r->dir = g_dir_make_tmp("gowl-webmail-XXXXXX", NULL);
	g_assert_nonnull(r->dir);
	r->run_dir     = g_build_filename(r->dir, "run", NULL);
	r->profile_dir = g_build_filename(r->dir, "profile", NULL);
	r->argv_log    = g_build_filename(r->dir, "argv.log", NULL);

	body = g_strdup_printf(
		"printf '%%s\\n' \"$@\" -- >> '%s'\n%s", r->argv_log,
		(fake_body != NULL) ? fake_body
		                    : "while :; do sleep 0.1; done");
	r->fake = bar_harness_script(&r->h, "fake-gsurf", body);

	cmd = g_strdup_printf("%s -c {config} {url}", r->fake);
	bar_harness_set(&r->h, "runtime-dir", r->run_dir);
	bar_harness_set(&r->h, "profile-dir", r->profile_dir);
	bar_harness_set(&r->h, "browser-command", cmd);
	bar_harness_activate(&r->h);
	return TRUE;
}

static void
rig_down(Rig *r)
{
	g_autofree gchar *rm = NULL;
	GPid pid;

	/* Never leave a stand-in browser behind. */
	pid = read_pid(r->run_dir);
	if (pid > 1)
		kill(pid, SIGTERM);
	bar_harness_down(&r->h);
	rm = g_strdup_printf("rm -rf '%s'", r->dir);
	if (system(rm) != 0)
		g_test_message("could not remove %s", r->dir);
	g_free(r->dir);
	g_free(r->run_dir);
	g_free(r->profile_dir);
	g_free(r->argv_log);
	g_free(r->fake);
}

static void
left_click(Rig *r)
{
	gowl_bar_plugin_on_click(r->h.plugin, BTN_LEFT, 0, 0, 0);
}

/* ================================================================
 * Titles -> unread counts
 * ================================================================ */

/*
 * Every title shape a supported web mail actually uses.  Would catch:
 * the parser missing a provider's format, or the userscript's "(N) "
 * prefix and Gmail's "Inbox (N)" disagreeing about what is read.
 */
static void
test_unread_formats(void)
{
	static const struct { const gchar *title; gint want; } cases[] = {
		/* Outlook, after the userscript, with gsurf's suffix */
		{ "(3) Mail - Zach Podbielniak - Outlook \xe2\x80\x94 gsurf", 3 },
		/* Gmail */
		{ "Inbox (12) - me@gmail.com - Gmail", 12 },
		{ "Inbox (1,234) - me@gmail.com - Gmail", 1234 },
		/* Fastmail */
		{ "Fastmail \xe2\x80\x93 Inbox (5)", 5 },
		/* Proton */
		{ "Inbox (7) | me@proton.me | Proton Mail", 7 },
		/* Roundcube */
		{ "(2) Roundcube Webmail :: Inbox", 2 },
		/* An explicit zero is a count, not "unknown" */
		{ "(0) Mail - Outlook", 0 },
		/* Case does not matter for the folder name */
		{ "INBOX (9) - Gmail", 9 },
	};
	guint i;

	for (i = 0; i < G_N_ELEMENTS(cases); i++) {
		gboolean plus = TRUE;

		g_assert_cmpint(bar_webmail_parse_unread(cases[i].title, &plus),
		                ==, cases[i].want);
		g_assert_false(plus);
	}
}

/*
 * Titles that are NOT counts.  Would catch: a subject line's "(2024)"
 * showing as two thousand unread messages, or the word "inbox" inside
 * another word being trusted.
 */
static void
test_unread_none(void)
{
	static const gchar *const titles[] = {
		"Mail - Zach Podbielniak - Outlook",
		"Re: Budget (2024) - me@gmail.com - Gmail",
		"Sandboxinbox (3)",
		"(abc) Inbox",
		"Inbox () - Gmail",
		"( 3 - not closed",
		"3 unread",
		"",
	};
	guint i;

	for (i = 0; i < G_N_ELEMENTS(titles); i++)
		g_assert_cmpint(bar_webmail_parse_unread(titles[i], NULL), ==, -1);
	g_assert_cmpint(bar_webmail_parse_unread(NULL, NULL), ==, -1);
}

/*
 * Capped counts.  Would catch: "(999+)" read as nothing, or a hostile
 * title with a twenty-digit number overflowing the integer.
 */
static void
test_unread_capped(void)
{
	gboolean plus = FALSE;

	g_assert_cmpint(bar_webmail_parse_unread("(999+) Mail", &plus), ==, 999);
	g_assert_true(plus);
	plus = FALSE;
	g_assert_cmpint(bar_webmail_parse_unread(
		"(123456789012345678901234567890) x", &plus), ==, 99999);
	g_assert_true(plus);
}

/*
 * Unicode.  Would catch: a right-to-left mark at the start of the
 * title hiding the count, a localised inbox word not matching, digits
 * from another script being refused, and bytes that are not UTF-8
 * reaching the parser at all.
 */
static void
test_unread_unicode(void)
{
	/* U+200F RIGHT-TO-LEFT MARK, then the count */
	g_assert_cmpint(bar_webmail_parse_unread(
		"\xe2\x80\x8f(4) \xd8\xa8\xd8\xb1\xd9\x8a\xd8\xaf", NULL), ==, 4);
	/* Japanese Gmail: 受信トレイ (5) */
	g_assert_cmpint(bar_webmail_parse_unread(
		"\xe5\x8f\x97\xe4\xbf\xa1\xe3\x83\x88\xe3\x83\xac\xe3\x82\xa4"
		" (5) - Gmail", NULL), ==, 5);
	/* German, upper case: POSTEINGANG (8) */
	g_assert_cmpint(bar_webmail_parse_unread("POSTEINGANG (8) - Gmail",
	                                         NULL), ==, 8);
	/* Arabic-Indic digit three */
	g_assert_cmpint(bar_webmail_parse_unread("(\xd9\xa3) x", NULL), ==, 3);
	/* An emoji-laden account name does not get in the way */
	g_assert_cmpint(bar_webmail_parse_unread(
		"Inbox (6) - \xf0\x9f\x93\xa7 me - Gmail", NULL), ==, 6);
	/* Not UTF-8 */
	g_assert_cmpint(bar_webmail_parse_unread("\xff(3) x", NULL), ==, -1);
}

/* ================================================================
 * URLs and providers
 * ================================================================ */

/*
 * Would catch: a work tenant on outlook.office.com not being treated
 * as Outlook (so no Fluent theme and no compose link), or a lookalike
 * host (evil-outlook.office.com.example) being trusted.
 */
static void
test_providers(void)
{
	g_assert_cmpint(bar_webmail_provider("https://outlook.cloud.microsoft/mail/"),
	                ==, BAR_WEBMAIL_PROVIDER_OUTLOOK);
	g_assert_cmpint(bar_webmail_provider("https://outlook.live.com/mail/0/"),
	                ==, BAR_WEBMAIL_PROVIDER_OUTLOOK);
	g_assert_cmpint(bar_webmail_provider("https://OUTLOOK.OFFICE.COM/mail/"),
	                ==, BAR_WEBMAIL_PROVIDER_OUTLOOK);
	g_assert_cmpint(bar_webmail_provider("https://mail.google.com/mail/u/0/"),
	                ==, BAR_WEBMAIL_PROVIDER_GMAIL);
	g_assert_cmpint(bar_webmail_provider("https://app.fastmail.com/mail/"),
	                ==, BAR_WEBMAIL_PROVIDER_FASTMAIL);
	g_assert_cmpint(bar_webmail_provider("https://mail.proton.me/u/0/inbox"),
	                ==, BAR_WEBMAIL_PROVIDER_PROTON);
	g_assert_cmpint(bar_webmail_provider("https://mail.example.org/roundcube/"),
	                ==, BAR_WEBMAIL_PROVIDER_GENERIC);
	g_assert_cmpint(bar_webmail_provider(
		"https://outlook.office.com.example/mail/"),
		==, BAR_WEBMAIL_PROVIDER_GENERIC);
	g_assert_cmpint(bar_webmail_provider("https://notoutlook.live.com.evil/"),
	                ==, BAR_WEBMAIL_PROVIDER_GENERIC);
	g_assert_cmpint(bar_webmail_provider("not a url"), ==,
	                BAR_WEBMAIL_PROVIDER_GENERIC);
	g_assert_cmpint(bar_webmail_provider(NULL), ==,
	                BAR_WEBMAIL_PROVIDER_GENERIC);
}

/*
 * Compose deep links per provider, on the configured host.  Would
 * catch: a personal outlook.live.com account composing on the work
 * host, or a provider with no known link getting a made-up one.
 */
static void
test_compose_urls(void)
{
	static const struct { const gchar *url; const gchar *want; } cases[] = {
		{ "https://outlook.cloud.microsoft/mail/",
		  "https://outlook.cloud.microsoft/mail/deeplink/compose" },
		{ "https://outlook.live.com/mail/0/",
		  "https://outlook.live.com/mail/deeplink/compose" },
		{ "https://mail.google.com/mail/u/0/#inbox",
		  "https://mail.google.com/mail/?view=cm&fs=1" },
		{ "https://app.fastmail.com/mail/Inbox/",
		  "https://app.fastmail.com/action/compose/" },
		{ "https://mail.proton.me/u/0/inbox",
		  "https://mail.proton.me/inbox#mailto=" },
	};
	guint i;

	for (i = 0; i < G_N_ELEMENTS(cases); i++) {
		g_autofree gchar *got = bar_webmail_compose_url(cases[i].url);

		g_assert_cmpstr(got, ==, cases[i].want);
	}
	g_assert_null(bar_webmail_compose_url("https://mail.example.org/"));
	g_assert_null(bar_webmail_compose_url(NULL));
}

/*
 * Would catch: the stylesheet leaking onto the sign-in hosts (scoped
 * to the mail host only), or Outlook moving between its hosts and
 * losing its theme.
 */
static void
test_style_patterns(void)
{
	g_auto(GStrv) outlook = NULL;
	g_auto(GStrv) other = NULL;

	outlook = bar_webmail_style_patterns("https://outlook.office.com/mail/");
	g_assert_cmpuint(g_strv_length(outlook), ==, 4);
	g_assert_true(g_strv_contains((const gchar * const *)outlook,
	                              "*://outlook.cloud.microsoft/*"));
	g_assert_true(g_strv_contains((const gchar * const *)outlook,
	                              "*://outlook.live.com/*"));

	other = bar_webmail_style_patterns("https://Mail.Example.org/rc/");
	g_assert_cmpuint(g_strv_length(other), ==, 1);
	g_assert_cmpstr(other[0], ==, "*://mail.example.org/*");

	g_assert_null(bar_webmail_style_patterns("nonsense"));
}

/* ================================================================
 * Command lines
 * ================================================================ */

/*
 * The URL is ONE argument whatever it contains.  Would catch: the
 * template being substituted before it is split, which turns a URL
 * with `&' or a space into several arguments -- or, through a shell,
 * into a second command.
 */
static void
test_argv_tokenisation(void)
{
	g_autoptr(GError) error = NULL;
	g_auto(GStrv) argv = NULL;
	const gchar *url = "https://mail.example.org/?a=1&b=two words;"
	                   " rm -rf ~ #frag";

	argv = bar_webmail_build_argv("gsurf --kiosk -c {config} {url}",
	                              "/run/user/1000/gowl-webmail/gsurf.yaml",
	                              url, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(g_strv_length(argv), ==, 5);
	g_assert_cmpstr(argv[0], ==, "gsurf");
	g_assert_cmpstr(argv[3], ==, "/run/user/1000/gowl-webmail/gsurf.yaml");
	g_assert_cmpstr(argv[4], ==, url);
}

/*
 * Template variants.  Would catch: a template without {url} opening a
 * blank window, a quoted path with spaces being split, a placeholder
 * inside an option losing its prefix, and an empty template doing
 * nothing instead of the default.
 */
static void
test_argv_templates(void)
{
	g_autoptr(GError) error = NULL;
	g_auto(GStrv) a = NULL;
	g_auto(GStrv) b = NULL;
	g_auto(GStrv) c = NULL;

	a = bar_webmail_build_argv("gsurf --kiosk", "/c.yaml",
	                           "https://x.test/", NULL);
	g_assert_cmpuint(g_strv_length(a), ==, 3);
	g_assert_cmpstr(a[2], ==, "https://x.test/");

	b = bar_webmail_build_argv("'/opt/my browser/gsurf' --config={config} "
	                           "--start={url}", "/c.yaml",
	                           "https://x.test/?q=a b", NULL);
	g_assert_cmpuint(g_strv_length(b), ==, 3);
	g_assert_cmpstr(b[0], ==, "/opt/my browser/gsurf");
	g_assert_cmpstr(b[1], ==, "--config=/c.yaml");
	g_assert_cmpstr(b[2], ==, "--start=https://x.test/?q=a b");

	c = bar_webmail_build_argv(NULL, "/c.yaml", "https://x.test/", NULL);
	g_assert_cmpstr(c[0], ==, "gsurf");
	g_assert_true(g_strv_contains((const gchar * const *)c, "--kiosk"));
	g_assert_true(g_strv_contains((const gchar * const *)c, "/c.yaml"));

	g_assert_null(bar_webmail_build_argv("gsurf 'unterminated", "/c",
	                                     "https://x.test/", &error));
	g_assert_nonnull(error);
}

/*
 * Would catch: a URL starting with `-' reaching the browser as its
 * last argument, where it is parsed as an option.
 */
static void
test_argv_refuses_option_url(void)
{
	g_autoptr(GError) error = NULL;

	g_assert_null(bar_webmail_build_argv(NULL, "/c", "--no-modules",
	                                     &error));
	g_assert_nonnull(error);
	g_assert_null(bar_webmail_build_argv(NULL, "/c", "", NULL));
	g_assert_null(bar_webmail_build_argv(NULL, "/c", NULL, NULL));
}

/*
 * The PID checks.  Would catch: a recycled PID being trusted because
 * its argv merely contains the config path as a substring, and a
 * process name with `) ' in it shifting the stat fields.
 */
static void
test_proc_parsing(void)
{
	static const gchar cmdline[] =
		"gsurf\0--kiosk\0-c\0/run/w/gsurf.yaml\0https://x/\0";
	static const gchar eq[] = "browser\0--config=/run/w/gsurf.yaml\0";

	g_assert_true(bar_webmail_cmdline_has_arg(cmdline, sizeof(cmdline) - 1,
	                                          "/run/w/gsurf.yaml"));
	g_assert_false(bar_webmail_cmdline_has_arg(cmdline, sizeof(cmdline) - 1,
	                                           "/run/w/gsurf"));
	g_assert_false(bar_webmail_cmdline_has_arg(cmdline, sizeof(cmdline) - 1,
	                                           "gsurf.yaml"));
	g_assert_true(bar_webmail_cmdline_has_arg(eq, sizeof(eq) - 1,
	                                          "/run/w/gsurf.yaml"));
	g_assert_false(bar_webmail_cmdline_has_arg(NULL, 0, "x"));

	g_assert_cmpint(bar_webmail_stat_ppid(
		"1234 (gsurf) S 77 1234 1234 0 -1"), ==, 77);
	g_assert_cmpint(bar_webmail_stat_ppid(
		"1234 (we) ird) S 88 1 1"), ==, 88);
	g_assert_cmpint(bar_webmail_stat_ppid("garbage"), ==, -1);
	g_assert_cmpint(bar_webmail_stat_ppid(NULL), ==, -1);
}

/* ================================================================
 * Colour and CSS
 * ================================================================ */

/*
 * Would catch: Latte getting a dark stylesheet (or Mocha a light one),
 * and a palette with no background guessing instead of saying it does
 * not know.
 */
static void
test_light_dark(void)
{
	BarWebmailPalette p;

	palette_mocha(&p);
	g_assert_cmpint(bar_webmail_palette_dark(&p), ==, 1);
	palette_latte(&p);
	g_assert_cmpint(bar_webmail_palette_dark(&p), ==, 0);

	memset(&p, 0, sizeof(p));
	g_assert_cmpint(bar_webmail_palette_dark(&p), ==, -1);
	palette_set(&p, BAR_WEBMAIL_PART_TEXT, "#f0f0f0");
	g_assert_cmpint(bar_webmail_palette_dark(&p), ==, 1);

	/* A mid grey ground: perceptually light (L* above 50). */
	memset(&p, 0, sizeof(p));
	palette_set(&p, BAR_WEBMAIL_PART_BACKGROUND, "#808080");
	g_assert_cmpint(bar_webmail_palette_dark(&p), ==, 0);
}

/*
 * Would catch: a short `#abc' or an alpha suffix being refused, and
 * junk being accepted as a colour.
 */
static void
test_hex_parsing(void)
{
	BarWebmailRgb c;

	g_assert_true(bar_webmail_parse_hex("#fff", &c));
	g_assert_cmpfloat_with_epsilon(c.r, 1.0, 1e-9);
	g_assert_true(bar_webmail_parse_hex("#89b4facc", &c));
	g_assert_cmpfloat_with_epsilon(c.b, 250.0 / 255.0, 1e-9);
	g_assert_false(bar_webmail_parse_hex("89b4fa", &c));
	g_assert_false(bar_webmail_parse_hex("#89b4fz", &c));
	g_assert_false(bar_webmail_parse_hex("#12345", &c));
	g_assert_false(bar_webmail_parse_hex("accent", &c));
	g_assert_false(bar_webmail_parse_hex(NULL, &c));
}

/*
 * The whole Outlook stylesheet for Mocha, and the generic one for
 * Latte, against golden files.  Would catch: a token renamed or
 * dropped, a blend changed by accident, the scoping selector for
 * Fluent's provider class lost.
 */
static void
test_css_golden(void)
{
	BarWebmailPalette p;
	g_autofree gchar *outlook = NULL;
	g_autofree gchar *generic = NULL;

	palette_mocha(&p);
	outlook = bar_webmail_css(&p, BAR_WEBMAIL_PROVIDER_OUTLOOK, NULL, NULL);
	assert_golden("outlook-mocha.css", outlook);

	palette_latte(&p);
	generic = bar_webmail_css(&p, BAR_WEBMAIL_PROVIDER_GENERIC,
		"body { font-size: 15px; }", "~/.config/gowl/webmail.css");
	assert_golden("generic-latte.css", generic);

	/* Spot checks that would survive a careless regold. */
	g_assert_nonnull(strstr(outlook, "--colorNeutralBackground1: #1e1e2e !important;"));
	g_assert_nonnull(strstr(outlook, "--colorBrandBackground: #89b4fa !important;"));
	g_assert_nonnull(strstr(outlook, "color-scheme: dark"));
	g_assert_nonnull(strstr(outlook, "fui-FluentProvider"));
	g_assert_nonnull(strstr(generic, "color-scheme: light"));
	g_assert_nonnull(strstr(generic, "body { font-size: 15px; }"));
	/* The extra CSS comes after everything generated, so it wins. */
	g_assert_true(strstr(generic, "font-size") > strstr(generic, "::selection"));
}

/*
 * Partial palettes.  Would catch: `follow-theme: false' with one
 * override still writing a whole theme (with blends made from
 * unset, all-black colours), and an empty palette writing anything
 * but the header.
 */
static void
test_css_partial(void)
{
	BarWebmailPalette p;
	g_autofree gchar *only_accent = NULL;
	g_autofree gchar *empty = NULL;

	memset(&p, 0, sizeof(p));
	palette_set(&p, BAR_WEBMAIL_PART_ACCENT, "#ff0000");
	only_accent = bar_webmail_css(&p, BAR_WEBMAIL_PROVIDER_OUTLOOK, NULL, NULL);
	g_assert_nonnull(strstr(only_accent, "--colorBrandBackground: #ff0000"));
	g_assert_null(strstr(only_accent, "--colorNeutralBackground1:"));
	/* Needs the background too, so it must be absent. */
	g_assert_null(strstr(only_accent, "--colorBrandBackground2:"));
	g_assert_null(strstr(only_accent, "color-scheme"));
	g_assert_null(strstr(only_accent, "#000000"));

	memset(&p, 0, sizeof(p));
	empty = bar_webmail_css(&p, BAR_WEBMAIL_PROVIDER_GENERIC, NULL, NULL);
	g_assert_null(strstr(empty, "{"));
}

/*
 * Would catch: the extra-css comment being closed early by a path
 * containing `*' `/', which would turn the user's first rule into
 * part of a comment or worse.
 */
static void
test_css_extra_name_cannot_close_comment(void)
{
	BarWebmailPalette p;
	g_autofree gchar *css = NULL;
	const gchar *mark;

	memset(&p, 0, sizeof(p));
	css = bar_webmail_css(&p, BAR_WEBMAIL_PROVIDER_GENERIC,
	                      "a { color: red; }", "/tmp/x*/evil.css");
	mark = strstr(css, "/* extra-css:");
	g_assert_nonnull(mark);
	g_assert_null(strstr(mark, "x*/"));
}

/*
 * Would catch: a palette switch going unnoticed (identical signatures)
 * or a no-op poll rewriting the files every two seconds (signatures
 * that differ for the same palette).
 */
static void
test_palette_signature(void)
{
	BarWebmailPalette a, b;
	g_autofree gchar *sa = NULL;
	g_autofree gchar *sa2 = NULL;
	g_autofree gchar *sb = NULL;
	g_autofree gchar *sc = NULL;

	palette_mocha(&a);
	palette_mocha(&b);
	sa  = bar_webmail_palette_signature(&a);
	sa2 = bar_webmail_palette_signature(&b);
	g_assert_cmpstr(sa, ==, sa2);

	palette_latte(&b);
	sb = bar_webmail_palette_signature(&b);
	g_assert_cmpstr(sa, !=, sb);

	/* Unsetting a part is a change too. */
	palette_mocha(&b);
	b.set[BAR_WEBMAIL_PART_BORDER] = FALSE;
	sc = bar_webmail_palette_signature(&b);
	g_assert_cmpstr(sa, !=, sc);
}

/* ================================================================
 * The gsurf configuration
 * ================================================================ */

/*
 * The generated config, golden.  Would catch: the cookie jar leaving
 * the private profile, a module the mail window must not have (modal,
 * history) being switched on, or a key gsurf reads being misspelt.
 */
static void
test_gsurf_config_golden(void)
{
	BarWebmailGsurfSpec spec;
	g_autofree gchar *yaml = NULL;

	memset(&spec, 0, sizeof(spec));
	spec.url          = "https://outlook.cloud.microsoft/mail/";
	spec.profile_dir  = "/home/u/.local/share/gowl-webmail";
	spec.cache_dir    = "/home/u/.cache/gowl-webmail";
	spec.css_path     = "/run/user/1000/gowl-webmail/theme.css";
	spec.js_path      = "/run/user/1000/gowl-webmail/unread.js";
	spec.download_dir = NULL;
	spec.dark         = 1;
	spec.third_party_cookies = TRUE;
	yaml = bar_webmail_gsurf_config(&spec);
	assert_golden("gsurf-outlook.yaml", yaml);

	g_assert_nonnull(strstr(yaml,
		"cookie_jar: \"/home/u/.local/share/gowl-webmail/cookies.sqlite\""));
	g_assert_nonnull(strstr(yaml, "  modal:\n    enabled: false"));
	g_assert_nonnull(strstr(yaml, "  history:\n    enabled: false"));
	g_assert_nonnull(strstr(yaml, "allow_web_notifications: allow"));
}

/*
 * Would catch: a URL with a newline or quote adding keys to the
 * browser's configuration, or `#' starting a YAML comment.
 */
static void
test_yaml_quoting(void)
{
	BarWebmailGsurfSpec spec;
	g_autofree gchar *q = NULL;
	g_autofree gchar *yaml = NULL;

	q = bar_webmail_yaml_quote("a\"b\\c\nmodules: x #y");
	g_assert_cmpstr(q, ==, "\"a\\\"b\\\\c\\x0amodules: x #y\"");

	memset(&spec, 0, sizeof(spec));
	spec.url = "https://x.test/\"\n  evil: true";
	spec.profile_dir = "/p";
	spec.cache_dir = "/c";
	spec.css_path = "/r/t.css";
	spec.js_path = "/r/u.js";
	spec.dark = -1;
	yaml = bar_webmail_gsurf_config(&spec);
	g_assert_null(strstr(yaml, "\n  evil: true"));
	g_assert_nonnull(strstr(yaml, "no-third-party"));
}

/*
 * Would catch: the userscript being emitted with a C-escape mistake
 * that leaves it unbalanced (it is JavaScript inside a C string).
 */
static void
test_userscript_shape(void)
{
	const gchar *js = bar_webmail_unread_script();
	gint depth = 0;
	const gchar *p;

	g_assert_nonnull(strstr(js, "document.title"));
	g_assert_nonnull(strstr(js, "aria-label"));
	g_assert_nonnull(strstr(js, "data-folder-name"));
	g_assert_true(g_utf8_validate(js, -1, NULL));
	for (p = js; *p != '\0'; p++) {
		if (*p == '{')
			depth++;
		else if (*p == '}')
			depth--;
		g_assert_cmpint(depth, >=, 0);
	}
	g_assert_cmpint(depth, ==, 0);
}

/* ================================================================
 * Decisions
 * ================================================================ */

/*
 * The click table.  Would catch: a click on a starting window
 * launching a second one, a focused window not hiding, and `show-on'
 * being ignored for a window on another workspace.
 */
static void
test_toggle_state_machine(void)
{
	g_assert_cmpint(bar_webmail_decide_toggle(BAR_WEBMAIL_WINDOW_NONE, TRUE),
	                ==, BAR_WEBMAIL_ACTION_LAUNCH);
	g_assert_cmpint(bar_webmail_decide_toggle(BAR_WEBMAIL_WINDOW_STARTING, TRUE),
	                ==, BAR_WEBMAIL_ACTION_WAIT);
	g_assert_cmpint(bar_webmail_decide_toggle(BAR_WEBMAIL_WINDOW_FOCUSED, TRUE),
	                ==, BAR_WEBMAIL_ACTION_HIDE);
	g_assert_cmpint(bar_webmail_decide_toggle(BAR_WEBMAIL_WINDOW_VISIBLE, TRUE),
	                ==, BAR_WEBMAIL_ACTION_FOCUS);
	g_assert_cmpint(bar_webmail_decide_toggle(BAR_WEBMAIL_WINDOW_MINIMIZED, TRUE),
	                ==, BAR_WEBMAIL_ACTION_RESTORE);
	g_assert_cmpint(bar_webmail_decide_toggle(BAR_WEBMAIL_WINDOW_ELSEWHERE, TRUE),
	                ==, BAR_WEBMAIL_ACTION_BRING);
	g_assert_cmpint(bar_webmail_decide_toggle(BAR_WEBMAIL_WINDOW_ELSEWHERE, FALSE),
	                ==, BAR_WEBMAIL_ACTION_JUMP);

	/* Show (right click, Open) never hides. */
	g_assert_cmpint(bar_webmail_decide_show(BAR_WEBMAIL_WINDOW_FOCUSED, TRUE),
	                ==, BAR_WEBMAIL_ACTION_FOCUS);
	g_assert_cmpint(bar_webmail_decide_show(BAR_WEBMAIL_WINDOW_MINIMIZED, FALSE),
	                ==, BAR_WEBMAIL_ACTION_RESTORE);
	g_assert_cmpint(bar_webmail_decide_show(BAR_WEBMAIL_WINDOW_NONE, FALSE),
	                ==, BAR_WEBMAIL_ACTION_LAUNCH);
}

/*
 * Colours and label by state.  Would catch: a stopped widget showing a
 * stale count, the unread colour applying with nothing unread, and a
 * four-digit count widening the bar.
 */
static void
test_look_and_label(void)
{
	g_autofree gchar *l1 = NULL;
	g_autofree gchar *l2 = NULL;
	g_autofree gchar *l3 = NULL;
	g_autofree gchar *l4 = NULL;

	g_assert_cmpint(bar_webmail_look(FALSE, 5), ==, BAR_WEBMAIL_LOOK_OFF);
	g_assert_cmpint(bar_webmail_look(TRUE, -1), ==, BAR_WEBMAIL_LOOK_ON);
	g_assert_cmpint(bar_webmail_look(TRUE, 0), ==, BAR_WEBMAIL_LOOK_ON);
	g_assert_cmpint(bar_webmail_look(TRUE, 2), ==, BAR_WEBMAIL_LOOK_UNREAD);

	g_assert_null(bar_webmail_label(0, FALSE, TRUE));
	g_assert_null(bar_webmail_label(-1, FALSE, TRUE));
	l1 = bar_webmail_label(3, FALSE, TRUE);
	g_assert_cmpstr(l1, ==, "\xe2\x97\x8f 3");
	l2 = bar_webmail_label(3, FALSE, FALSE);
	g_assert_cmpstr(l2, ==, "\xe2\x97\x8f");
	l3 = bar_webmail_label(4321, FALSE, TRUE);
	g_assert_cmpstr(l3, ==, "\xe2\x97\x8f 999+");
	l4 = bar_webmail_label(999, TRUE, TRUE);
	g_assert_cmpstr(l4, ==, "\xe2\x97\x8f 999+");
}

/* Would catch: Quit arming forever, or firing on a stale arm. */
static void
test_confirm_window(void)
{
	gint64 t = 1000 * G_TIME_SPAN_SECOND;

	g_assert_false(bar_webmail_confirm_armed(0, t));
	g_assert_true(bar_webmail_confirm_armed(t, t + G_TIME_SPAN_SECOND));
	g_assert_false(bar_webmail_confirm_armed(t, t + 6 * G_TIME_SPAN_SECOND));
	g_assert_false(bar_webmail_confirm_armed(t, t - 1));
}

/* ================================================================
 * The widget, in the harness
 * ================================================================ */

/*
 * Registration under all three names.  Would catch: an alias left out
 * of the registry, so `outlook' in a config shows nothing.
 */
static void
test_registration(void)
{
	BarHarness h;
	g_autoptr(GError) error = NULL;
	GowlBarPlugin *alias;

	if (!bar_harness_up(&h, "bar_register_webmail_plugins", "webmail"))
		return;
	alias = gowl_bar_registry_instantiate(h.registry, "outlook", &error);
	g_assert_no_error(error);
	g_assert_nonnull(alias);
	g_object_unref(alias);
	alias = gowl_bar_registry_instantiate(h.registry, "mail-web", &error);
	g_assert_no_error(error);
	g_assert_nonnull(alias);
	g_object_unref(alias);
	bar_harness_down(&h);
}

/*
 * A left click with nothing running launches the browser with the
 * generated files.  Would catch: the config/CSS/script not being
 * written before the browser starts, the runtime directory being
 * world-readable, the files holding session-adjacent data being
 * anything but 0600, and the URL not reaching the browser intact.
 */
static void
test_launch_writes_and_spawns(void)
{
	Rig r;
	g_autofree gchar *config = NULL;
	g_autofree gchar *css_path = NULL;
	g_autofree gchar *js_path = NULL;
	g_autofree gchar *cfg_path = NULL;
	g_autofree gchar *tip = NULL;
	g_autoptr(GowlBarPanel) panel = NULL;
	GStatBuf st;

	if (!rig_up(&r, NULL))
		return;
	bar_harness_poll(&r.h);
	g_assert_cmpint(gowl_bar_plugin_get_color(r.h.plugin), ==,
	                GOWL_BAR_COLOR_MUTED);
	g_assert_null(gowl_bar_plugin_dup_label(r.h.plugin));

	left_click(&r);
	g_assert_true(wait_for_lines(r.argv_log, 4));

	cfg_path = g_build_filename(r.run_dir, "gsurf.yaml", NULL);
	css_path = g_build_filename(r.run_dir, "theme.css", NULL);
	js_path  = g_build_filename(r.run_dir, "unread.js", NULL);
	g_assert_cmpuint(count_lines_with(r.argv_log, "-c"), ==, 1);
	g_assert_cmpuint(count_lines_with(r.argv_log, cfg_path), ==, 1);
	g_assert_cmpuint(count_lines_with(r.argv_log,
		"https://outlook.cloud.microsoft/mail/"), ==, 1);

	g_assert_cmpint(g_stat(r.run_dir, &st), ==, 0);
	g_assert_cmpint(st.st_mode & 0777, ==, 0700);
	g_assert_cmpint(g_stat(r.profile_dir, &st), ==, 0);
	g_assert_cmpint(st.st_mode & 0777, ==, 0700);
	g_assert_cmpint(g_stat(cfg_path, &st), ==, 0);
	g_assert_cmpint(st.st_mode & 0777, ==, 0600);
	g_assert_cmpint(g_stat(css_path, &st), ==, 0);
	g_assert_cmpint(st.st_mode & 0777, ==, 0600);
	g_assert_true(g_file_test(js_path, G_FILE_TEST_EXISTS));

	config = slurp(cfg_path);
	g_assert_nonnull(strstr(config, r.profile_dir));
	g_assert_nonnull(strstr(config, css_path));

	/* Running (no compositor here, so "starting"): normal colour. */
	bar_harness_poll(&r.h);
	g_assert_cmpint(gowl_bar_plugin_get_color(r.h.plugin), ==,
	                GOWL_BAR_COLOR_TEXT);
	tip = gowl_bar_plugin_dup_tooltip(r.h.plugin);
	g_assert_nonnull(strstr(tip, "starting"));
	panel = bar_harness_panel(&r.h);
	g_assert_true(bar_harness_panel_mentions(panel, "Starting"));

	/* A second click while it starts must not start a second one. */
	left_click(&r);
	g_usleep(100 * 1000);
	g_assert_cmpuint(count_lines_with(r.argv_log, "--"), ==, 1);

	rig_down(&r);
}

/*
 * Quit is two clicks and stops exactly our browser.  Would catch: Quit
 * acting on the first click, and the signal going to a PID without
 * checking it is still the browser we started.
 */
static void
test_quit_two_step(void)
{
	Rig r;
	g_autoptr(GowlBarPanel) armed = NULL;
	GPid pid;

	if (!rig_up(&r, NULL))
		return;
	left_click(&r);
	g_assert_true(wait_for_lines(r.argv_log, 4));
	pid = read_pid(r.run_dir);
	g_assert_cmpint(pid, >, 1);
	bar_harness_poll(&r.h);

	gowl_bar_plugin_panel_action(r.h.plugin, "tools", 1, 0.0, 1);
	armed = bar_harness_panel(&r.h);
	g_assert_true(bar_harness_panel_mentions(armed, "Click again to quit"));
	g_usleep(50 * 1000);
	g_assert_cmpint(kill(pid, 0), ==, 0);

	gowl_bar_plugin_panel_action(r.h.plugin, "tools", 1, 0.0, 1);
	g_assert_true(wait_dead(pid));

	bar_harness_poll(&r.h);
	g_assert_cmpint(gowl_bar_plugin_get_color(r.h.plugin), ==,
	                GOWL_BAR_COLOR_MUTED);
	rig_down(&r);
}

/*
 * The browser not installed.  Would catch: a click that silently does
 * nothing, and the panel not naming the package to install.
 */
static void
test_missing_browser(void)
{
	Rig r;
	g_autoptr(GowlBarPanel) panel = NULL;
	g_autofree gchar *tip = NULL;

	if (!rig_up(&r, NULL))
		return;
	bar_harness_set(&r.h, "browser-command",
	                "/nonexistent/gsurf -c {config} {url}");
	bar_harness_poll(&r.h);              /* the async half looks */
	bar_harness_poll(&r.h);

	tip = gowl_bar_plugin_dup_tooltip(r.h.plugin);
	g_assert_nonnull(strstr(tip, "not installed"));
	/* The click opens the panel instead of trying. */
	g_assert_false(gowl_bar_plugin_on_click(r.h.plugin, BTN_LEFT, 0, 0, 0));
	panel = bar_harness_panel(&r.h);
	g_assert_true(bar_harness_panel_mentions(panel, "webkit2gtk4.1"));
	g_assert_true(bar_harness_panel_mentions(panel, "/nonexistent/gsurf"));
	g_assert_false(g_file_test(r.argv_log, G_FILE_TEST_EXISTS));
	rig_down(&r);
}

/*
 * Only http(s) URLs.  Would catch: a `file:' or option-looking URL
 * being handed to a kiosk browser.
 */
static void
test_bad_url_refused(void)
{
	Rig r;
	g_autoptr(GowlBarPanel) panel = NULL;

	if (!rig_up(&r, NULL))
		return;
	bar_harness_set(&r.h, "url", "file:///etc/passwd");
	bar_harness_poll(&r.h);
	left_click(&r);
	panel = bar_harness_panel(&r.h);
	g_assert_true(bar_harness_panel_mentions(panel, "https://"));
	g_assert_false(g_file_test(r.argv_log, G_FILE_TEST_EXISTS));
	rig_down(&r);
}

/*
 * The CSS the widget writes follows the bar theme, per-part overrides
 * win, and `follow-theme: false' leaves the rest to the site.  Would
 * catch: an override losing to the palette, a role name not being
 * resolved through the theme, or follow-theme being ignored.
 */
static void
test_override_precedence(void)
{
	Rig r;
	g_autofree gchar *css_path = NULL;
	g_autofree gchar *css = NULL;
	g_autofree gchar *css2 = NULL;
	g_autofree gchar *base_hex = NULL;
	g_autofree gchar *red_hex = NULL;
	const gdouble *c;
	BarWebmailRgb rgb;
	GString *s;

	if (!rig_up(&r, NULL))
		return;
	css_path = g_build_filename(r.run_dir, "theme.css", NULL);

	/* What the stub host's theme says base and red are. */
	s = g_string_new(NULL);
	c = gowl_bar_theme_color(r.h.host->theme, GOWL_BAR_COLOR_BASE);
	rgb.r = c[0];
	rgb.g = c[1];
	rgb.b = c[2];
	append_hex(s, &rgb);
	base_hex = g_string_free(s, FALSE);
	s = g_string_new(NULL);
	c = gowl_bar_theme_color(r.h.host->theme, GOWL_BAR_COLOR_RED);
	rgb.r = c[0];
	rgb.g = c[1];
	rgb.b = c[2];
	append_hex(s, &rgb);
	red_hex = g_string_free(s, FALSE);

	/* Palette, with the accent overridden by hex and the border by
	   role name. */
	bar_harness_set(&r.h, "color-accent", "#ff8800");
	bar_harness_set(&r.h, "color-border", "red");
	gowl_bar_plugin_panel_action(r.h.plugin, "tools", 0, 0.0, 1);
	css = slurp(css_path);
	{
		g_autofree gchar *want_bg = g_strdup_printf(
			"--colorNeutralBackground1: %s !important;", base_hex);
		g_autofree gchar *want_border = g_strdup_printf(
			"--colorNeutralStroke1: %s !important;", red_hex);

		g_assert_nonnull(strstr(css, want_bg));
		g_assert_nonnull(strstr(css, want_border));
	}
	g_assert_nonnull(strstr(css, "--colorBrandBackground: #ff8800 !important;"));

	/* follow-theme off: only the overrides remain. */
	bar_harness_set(&r.h, "follow-theme", "false");
	gowl_bar_plugin_panel_action(r.h.plugin, "tools", 0, 0.0, 1);
	css2 = slurp(css_path);
	g_assert_null(strstr(css2, "--colorNeutralBackground1:"));
	g_assert_nonnull(strstr(css2, "--colorBrandBackground: #ff8800 !important;"));
	/* An unset part no longer comes from the palette. */
	g_assert_null(strstr(css2, "--colorNeutralForeground1:"));
	/* The role-name override still resolves through the theme. */
	{
		g_autofree gchar *want_border = g_strdup_printf(
			"--colorNeutralStroke1: %s !important;", red_hex);

		g_assert_nonnull(strstr(css2, want_border));
	}

	/* Nothing was launched by rewriting the theme. */
	g_assert_false(g_file_test(r.argv_log, G_FILE_TEST_EXISTS));
	rig_down(&r);
}

/*
 * The bar label's own colour settings.  Would catch: color-off being
 * read as a role only (a hex literal silently dropped), and the
 * unread colour applying while the window is not running.
 */
static void
test_label_colour_settings(void)
{
	Rig r;
	gdouble rgba[4];

	if (!rig_up(&r, NULL))
		return;
	bar_harness_set(&r.h, "color-off", "#112233");
	bar_harness_poll(&r.h);
	g_assert_true(gowl_bar_plugin_get_color_rgba(r.h.plugin, rgba));
	g_assert_cmpfloat_with_epsilon(rgba[0], (gdouble)0x11 / 255.0, 1e-6);
	g_assert_cmpfloat_with_epsilon(rgba[2], (gdouble)0x33 / 255.0, 1e-6);

	bar_harness_set(&r.h, "color-off", "peach");
	bar_harness_poll(&r.h);
	g_assert_false(gowl_bar_plugin_get_color_rgba(r.h.plugin, rgba));
	g_assert_cmpint(gowl_bar_plugin_get_color(r.h.plugin), ==,
	                GOWL_BAR_COLOR_PEACH);

	bar_harness_set(&r.h, "color-on", "green");
	left_click(&r);
	g_assert_true(wait_for_lines(r.argv_log, 4));
	bar_harness_poll(&r.h);
	g_assert_cmpint(gowl_bar_plugin_get_color(r.h.plugin), ==,
	                GOWL_BAR_COLOR_GREEN);
	rig_down(&r);
}

/*
 * A browser that keeps dying is relaunched a few times and then left
 * alone, with one toast saying so.  Would catch: run-in-background
 * relaunching a crashing browser forever, once every poll.
 */
static void
test_background_relaunch_brake(void)
{
	Rig r;
	guint i;

	if (!rig_up(&r, "exit 0"))
		return;
	bar_harness_set(&r.h, "run-in-background", "true");
	left_click(&r);
	for (i = 1; i <= WM_TEST_MAX_LAUNCHES; i++) {
		GPid pid;

		g_assert_true(wait_for_lines(r.argv_log, 4 * i));
		pid = read_pid(r.run_dir);
		g_assert_true(wait_dead(pid));
		bar_harness_poll(&r.h);
	}
	g_usleep(200 * 1000);
	g_assert_cmpuint(count_lines_with(r.argv_log, "--"), ==,
	                 WM_TEST_MAX_LAUNCHES);
	g_assert_true(bar_harness_toasted(&r.h, "keeps exiting"));
	rig_down(&r);
}

/*
 * A reloaded bar adopts the window the previous one started, and
 * refuses a PID file pointing at somebody else's process.  Would
 * catch: a bar reload starting a second mail window, and a recycled
 * PID being adopted (and later signalled).
 */
static void
test_pid_restore(void)
{
	Rig r;
	BarHarness h2;
	g_autofree gchar *cmd = NULL;
	g_autofree gchar *pid_path = NULL;
	g_autofree gchar *own = NULL;
	GPid pid;

	if (!rig_up(&r, NULL))
		return;
	left_click(&r);
	g_assert_true(wait_for_lines(r.argv_log, 4));
	pid = read_pid(r.run_dir);

	/* A second widget instance on the same runtime directory. */
	g_assert_true(bar_harness_up(&h2, "bar_register_webmail_plugins",
	                             "webmail"));
	cmd = g_strdup_printf("%s -c {config} {url}", r.fake);
	bar_harness_set(&h2, "runtime-dir", r.run_dir);
	bar_harness_set(&h2, "profile-dir", r.profile_dir);
	bar_harness_set(&h2, "browser-command", cmd);
	bar_harness_activate(&h2);
	bar_harness_poll(&h2);
	g_assert_cmpint(gowl_bar_plugin_get_color(h2.plugin), ==,
	                GOWL_BAR_COLOR_TEXT);
	bar_harness_down(&h2);

	/* Our own PID is a live process, but not the browser. */
	kill(pid, SIGTERM);
	g_assert_true(wait_dead(pid));
	pid_path = g_build_filename(r.run_dir, "pid", NULL);
	own = g_strdup_printf("%d\n", (gint)getpid());
	g_assert_true(g_file_set_contents(pid_path, own, -1, NULL));
	g_assert_true(bar_harness_up(&h2, "bar_register_webmail_plugins",
	                             "webmail"));
	bar_harness_set(&h2, "runtime-dir", r.run_dir);
	bar_harness_activate(&h2);
	bar_harness_poll(&h2);
	g_assert_cmpint(gowl_bar_plugin_get_color(h2.plugin), ==,
	                GOWL_BAR_COLOR_MUTED);
	bar_harness_down(&h2);
	g_assert_true(g_file_set_contents(pid_path, "0\n", -1, NULL));
	rig_down(&r);
}

/*
 * Compose.  Would catch: a provider with no compose page opening the
 * inbox and pretending, and the compose window replacing the tracked
 * main window's PID.
 */
static void
test_compose(void)
{
	Rig r;

	/* The stand-in exits on its own: a compose window is not tracked,
	   so nothing else would stop it. */
	if (!rig_up(&r, "sleep 0.3"))
		return;
	bar_harness_set(&r.h, "url", "https://mail.example.org/");
	gowl_bar_plugin_on_click(r.h.plugin, BTN_MIDDLE, 0, 0, 0);
	g_assert_true(bar_harness_toasted(&r.h, "No compose page"));
	g_assert_false(g_file_test(r.argv_log, G_FILE_TEST_EXISTS));

	bar_harness_set(&r.h, "url", "https://outlook.office.com/mail/");
	gowl_bar_plugin_on_click(r.h.plugin, BTN_MIDDLE, 0, 0, 0);
	g_assert_true(wait_for_lines(r.argv_log, 4));
	g_assert_cmpuint(count_lines_with(r.argv_log,
		"https://outlook.office.com/mail/deeplink/compose"), ==, 1);
	/* Untracked: the widget still thinks nothing is running. */
	g_assert_cmpint(read_pid(r.run_dir), ==, 0);
	bar_harness_poll(&r.h);
	g_assert_cmpint(gowl_bar_plugin_get_color(r.h.plugin), ==,
	                GOWL_BAR_COLOR_MUTED);

	/* The explicit setting wins over the derived link. */
	bar_harness_set(&r.h, "compose-url", "https://mail.example.org/new");
	gowl_bar_plugin_on_click(r.h.plugin, BTN_MIDDLE, 0, 0, 0);
	g_assert_true(wait_for_lines(r.argv_log, 8));
	g_assert_cmpuint(count_lines_with(r.argv_log,
		"https://mail.example.org/new"), ==, 1);
	rig_down(&r);
}

/*
 * Shift-click opens the panel.  Would catch: the only pointer path to
 * the panel (Quit, Reload theme) being swallowed by the toggle.
 */
static void
test_modified_click_opens_panel(void)
{
	Rig r;

	if (!rig_up(&r, NULL))
		return;
	g_assert_false(gowl_bar_plugin_on_click(r.h.plugin, BTN_LEFT, 0, 0,
	                                        WLR_TEST_MODIFIER_SHIFT));
	g_assert_false(g_file_test(r.argv_log, G_FILE_TEST_EXISTS));
	bar_harness_set(&r.h, "click", "panel");
	g_assert_false(gowl_bar_plugin_on_click(r.h.plugin, BTN_LEFT, 0, 0, 0));
	g_assert_false(g_file_test(r.argv_log, G_FILE_TEST_EXISTS));
	rig_down(&r);
}

int
main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/bar-webmail/unread/formats", test_unread_formats);
	g_test_add_func("/bar-webmail/unread/none", test_unread_none);
	g_test_add_func("/bar-webmail/unread/capped", test_unread_capped);
	g_test_add_func("/bar-webmail/unread/unicode", test_unread_unicode);
	g_test_add_func("/bar-webmail/url/providers", test_providers);
	g_test_add_func("/bar-webmail/url/compose", test_compose_urls);
	g_test_add_func("/bar-webmail/url/style-patterns", test_style_patterns);
	g_test_add_func("/bar-webmail/argv/tokenisation", test_argv_tokenisation);
	g_test_add_func("/bar-webmail/argv/templates", test_argv_templates);
	g_test_add_func("/bar-webmail/argv/option-url",
	                test_argv_refuses_option_url);
	g_test_add_func("/bar-webmail/proc/parsing", test_proc_parsing);
	g_test_add_func("/bar-webmail/colour/light-dark", test_light_dark);
	g_test_add_func("/bar-webmail/colour/hex", test_hex_parsing);
	g_test_add_func("/bar-webmail/css/golden", test_css_golden);
	g_test_add_func("/bar-webmail/css/partial", test_css_partial);
	g_test_add_func("/bar-webmail/css/extra-name",
	                test_css_extra_name_cannot_close_comment);
	g_test_add_func("/bar-webmail/css/signature", test_palette_signature);
	g_test_add_func("/bar-webmail/config/golden", test_gsurf_config_golden);
	g_test_add_func("/bar-webmail/config/quoting", test_yaml_quoting);
	g_test_add_func("/bar-webmail/config/userscript", test_userscript_shape);
	g_test_add_func("/bar-webmail/decide/toggle", test_toggle_state_machine);
	g_test_add_func("/bar-webmail/decide/look", test_look_and_label);
	g_test_add_func("/bar-webmail/decide/confirm", test_confirm_window);
	g_test_add_func("/bar-webmail/widget/registration", test_registration);
	g_test_add_func("/bar-webmail/widget/launch",
	                test_launch_writes_and_spawns);
	g_test_add_func("/bar-webmail/widget/quit", test_quit_two_step);
	g_test_add_func("/bar-webmail/widget/missing", test_missing_browser);
	g_test_add_func("/bar-webmail/widget/bad-url", test_bad_url_refused);
	g_test_add_func("/bar-webmail/widget/overrides",
	                test_override_precedence);
	g_test_add_func("/bar-webmail/widget/label-colours",
	                test_label_colour_settings);
	g_test_add_func("/bar-webmail/widget/relaunch-brake",
	                test_background_relaunch_brake);
	g_test_add_func("/bar-webmail/widget/pid-restore", test_pid_restore);
	g_test_add_func("/bar-webmail/widget/compose", test_compose);
	g_test_add_func("/bar-webmail/widget/modified-click",
	                test_modified_click_opens_panel);

	return g_test_run();
}
