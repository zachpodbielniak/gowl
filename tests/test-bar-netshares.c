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
 * The `netshares' widget (modules/bar/bar-plugins-netshares.c and
 * bar-netshares-model.c), offline, against stand-ins for pkexec,
 * secret-tool, sshfs and fusermount3 that record their argv, their
 * environment and their stdin into a scratch directory, and a fake
 * mount table they edit the way a real mount would.
 *
 * Asserted:
 *
 *   A PASSWORD NEVER TRAVELS IN ARGV OR THE ENVIRONMENT.  It reaches
 *   mount.cifs only through a 0600 credentials file, which the stand-in
 *   pkexec stats WHILE it runs, and which is gone when mount returns;
 *   it reaches the keyring on secret-tool's stdin.
 *
 *   HOSTILE CONFIGURATION IS REFUSED, ONE ENTRY AT A TIME.  A comma in
 *   a share, a `suid' or `credentials=' extra, a server that is an
 *   option, a mount point that climbs out with `..' or a symlink --
 *   each costs its own entry, with a reason, and nothing else.
 *
 *   THE COMMAND LINES ARE EXACT.  Each type's argv is compared whole.
 *
 *   NOTHING WAITS FOREVER.  A helper that hangs is killed at the
 *   timeout; a child that will not read its stdin does not block the
 *   write, and one that exits early does not SIGPIPE the compositor.
 */

#include <glib.h>
#include <glib/gstdio.h>
#include <errno.h>
#include <signal.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define BAR_HARNESS_MODULE GOWL_TEST_BAR_MODULE
#include "bar-widget-harness.h"
#include "modules/bar/bar-netshares-model.h"

#ifndef GOWL_TEST_BAR_MODULE
#define GOWL_TEST_BAR_MODULE "build/release/modules/bar.so"
#endif
#ifndef GOWL_TEST_FIXTURES
#define GOWL_TEST_FIXTURES "tests/fixtures/bar"
#endif

/* ── The model, reached through bar.so ────────────────────────────── */

typedef struct {
	__typeof__(bar_netshares_load)               *load;
	__typeof__(bar_netshares_to_yaml)            *to_yaml;
	__typeof__(bar_netshares_find)               *find;
	__typeof__(bar_netshares_slug)               *slug;
	__typeof__(bar_netshares_share_new)          *share_new;
	__typeof__(bar_netshares_share_free)         *share_free;
	__typeof__(bar_netshares_validate)           *validate;
	__typeof__(bar_netshares_option_ok)          *option_ok;
	__typeof__(bar_netshares_resolve_mountpoint) *resolve;
	__typeof__(bar_netshares_parse_mountinfo)    *parse_mountinfo;
	__typeof__(bar_netshares_find_mount)         *find_mount;
	__typeof__(bar_netshares_mount_argv)         *mount_argv;
	__typeof__(bar_netshares_unmount_argv)       *unmount_argv;
	__typeof__(bar_netshares_secret_argv)        *secret_argv;
	__typeof__(bar_netshares_credentials_text)   *cred_text;
	__typeof__(bar_netshares_write_credentials)  *write_cred;
	__typeof__(bar_netshares_run)                *run;
	__typeof__(bar_netshares_run_clear)          *run_clear;
	__typeof__(bar_netshares_last_line)          *last_line;
	__typeof__(bar_netshares_is_busy)            *is_busy;
	__typeof__(bar_netshares_package_for)        *package_for;
} Api;

static Api api;

static gboolean
api_up(BarHarness *h)
{
#define SYM(field, name) \
	api.field = bar_harness_sym(h, "bar_netshares_" name); \
	if (api.field == NULL) { \
		g_test_skip("bar.so lacks bar_netshares_" name); \
		return FALSE; \
	}
	SYM(load,            "load");
	SYM(to_yaml,         "to_yaml");
	SYM(find,            "find");
	SYM(slug,            "slug");
	SYM(share_new,       "share_new");
	SYM(share_free,      "share_free");
	SYM(validate,        "validate");
	SYM(option_ok,       "option_ok");
	SYM(resolve,         "resolve_mountpoint");
	SYM(parse_mountinfo, "parse_mountinfo");
	SYM(find_mount,      "find_mount");
	SYM(mount_argv,      "mount_argv");
	SYM(unmount_argv,    "unmount_argv");
	SYM(secret_argv,     "secret_argv");
	SYM(cred_text,       "credentials_text");
	SYM(write_cred,      "write_credentials");
	SYM(run,             "run");
	SYM(run_clear,       "run_clear");
	SYM(last_line,       "last_line");
	SYM(is_busy,         "is_busy");
	SYM(package_for,     "package_for");
#undef SYM
	return TRUE;
}

/* Load bar.so for the model alone (no widget). */
static gboolean
model_up(BarHarness *h)
{
	if (!bar_harness_up(h, "bar_register_netshares_plugins", NULL))
		return FALSE;
	return api_up(h);
}

static gchar *
fixture_text(const gchar *name)
{
	g_autofree gchar *path = NULL;
	gchar *text = NULL;

	path = g_build_filename(GOWL_TEST_FIXTURES, "netshares", name, NULL);
	g_assert_true(g_file_get_contents(path, &text, NULL, NULL));
	return text;
}

static gchar *
read_or_empty(const gchar *path)
{
	gchar *text = NULL;

	if (!g_file_get_contents(path, &text, NULL, NULL))
		return g_strdup("");
	return text;
}

/* Compare an argv against an expected one, element by element, so a
   failure names the first difference rather than "not equal". */
static void
assert_argv(gchar **got, const gchar *const *want)
{
	guint i;

	for (i = 0; want[i] != NULL; i++) {
		g_assert_nonnull(got[i]);
		g_assert_cmpstr(got[i], ==, want[i]);
	}
	g_assert_null(got[i]);
}

/* ── Loading ──────────────────────────────────────────────────────── */

/* Catches: a field read from the wrong key (`export'/`path' are the
   share field for NFS/SSHFS), a default that is not the safe one
   (encrypt and noexec on), an option list that is dropped. */
static void
test_load_valid(void)
{
	BarHarness h;
	g_autofree gchar *text = NULL;
	g_autoptr(GPtrArray) shares = NULL;
	g_autoptr(GPtrArray) warnings = NULL;
	g_autoptr(GError) error = NULL;
	BarNetshare *s;

	if (!model_up(&h))
		return;
	text = fixture_text("valid.yaml");
	shares = api.load(text, &warnings, &error);
	g_assert_no_error(error);
	g_assert_nonnull(shares);
	g_assert_cmpuint(shares->len, ==, 3);
	g_assert_cmpuint(warnings->len, ==, 0);

	s = api.find(shares, "nas-media");
	g_assert_nonnull(s);
	g_assert_cmpint(s->type, ==, BAR_NETSHARES_CIFS);
	g_assert_cmpstr(s->user, ==, "WORKGROUP\\zach");
	g_assert_cmpstr(s->share, ==, "media");
	g_assert_true(s->encrypt);
	g_assert_true(s->noexec);
	g_assert_true(s->automount);
	g_assert_cmpstr(s->extra[0], ==, "vers=3.1.1");

	s = api.find(shares, "backup");
	g_assert_cmpint(s->type, ==, BAR_NETSHARES_NFS);
	g_assert_cmpstr(s->share, ==, "/srv/backup");
	g_assert_true(s->readonly);
	g_assert_cmpint(s->port, ==, 2049);
	g_assert_cmpstr(s->nfs_version, ==, "4.2");

	s = api.find(shares, "devbox");
	g_assert_cmpint(s->type, ==, BAR_NETSHARES_SSHFS);
	g_assert_cmpstr(s->share, ==, "/home/zach/src");
	g_assert_false(s->noexec);
	g_assert_cmpint(s->port, ==, 2222);

	/* And it survives a round trip through the writer unchanged. */
	{
		g_autofree gchar *yaml = api.to_yaml(shares);
		g_autoptr(GPtrArray) again = NULL;
		g_autoptr(GPtrArray) warn2 = NULL;
		BarNetshare *a, *b;
		guint i;

		again = api.load(yaml, &warn2, &error);
		g_assert_no_error(error);
		g_assert_cmpuint(again->len, ==, 3);
		g_assert_cmpuint(warn2->len, ==, 0);
		for (i = 0; i < 3; i++) {
			a = g_ptr_array_index(shares, i);
			b = g_ptr_array_index(again, i);
			g_assert_cmpstr(a->id, ==, b->id);
			g_assert_cmpstr(a->user, ==, b->user);
			g_assert_cmpstr(a->share, ==, b->share);
			g_assert_cmpint(a->port, ==, b->port);
			g_assert_cmpint(a->readonly, ==, b->readonly);
			g_assert_cmpint(a->noexec, ==, b->noexec);
		}
	}
	bar_harness_down(&h);
}

/* Catches: an empty or absent file treated as an error (the first run
   would show a red panel), and a malformed file that silently loads as
   nothing (the user's shares vanish without a word). */
static void
test_load_empty_and_malformed(void)
{
	BarHarness h;
	g_autofree gchar *bad = NULL;
	g_autoptr(GPtrArray) shares = NULL;
	g_autoptr(GPtrArray) none = NULL;
	g_autoptr(GPtrArray) list = NULL;
	g_autoptr(GError) error = NULL;

	if (!model_up(&h))
		return;

	none = api.load("", NULL, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(none->len, ==, 0);

	bad = fixture_text("malformed.yaml");
	shares = api.load(bad, NULL, &error);
	g_assert_null(shares);
	g_assert_nonnull(error);
	g_clear_error(&error);

	/* Right syntax, wrong shape. */
	list = api.load("connections: just-a-string\n", NULL, &error);
	g_assert_null(list);
	g_assert_nonnull(error);
	g_clear_error(&error);

	/* Not UTF-8 at all. */
	g_assert_null(api.load("connections:\n  - name: \xff\xfe\n", NULL,
	                       &error));
	g_assert_nonnull(error);
	bar_harness_down(&h);
}

static gboolean
warned(GPtrArray *warnings, const gchar *needle)
{
	guint i;

	for (i = 0; i < warnings->len; i++)
		if (strstr(g_ptr_array_index(warnings, i), needle) != NULL)
			return TRUE;
	return FALSE;
}

/* Catches: any of the injection routes into a root `mount' -- a comma
   that turns a share into an option, a quote, an extra that re-enables
   suid or names a credentials file, a server that is an option, a
   mount point that climbs out, a newline that adds a key to the
   credentials file -- and a refusal that takes the good entries down
   with it. */
static void
test_load_hostile(void)
{
	BarHarness h;
	g_autofree gchar *text = NULL;
	g_autoptr(GPtrArray) shares = NULL;
	g_autoptr(GPtrArray) warnings = NULL;
	g_autoptr(GError) error = NULL;
	guint i;

	if (!model_up(&h))
		return;
	text = fixture_text("hostile.yaml");
	shares = api.load(text, &warnings, &error);
	g_assert_no_error(error);

	/* Exactly one survives, and it is the one with spaces. */
	g_assert_cmpuint(shares->len, ==, 1);
	g_assert_cmpstr(((BarNetshare *)g_ptr_array_index(shares, 0))->share,
	                ==, "Shared Music");

	for (i = 0; i < warnings->len; i++)
		g_test_message("warning: %s",
		               (gchar *)g_ptr_array_index(warnings, i));

	g_assert_true(warned(warnings, "comma-share"));
	g_assert_true(warned(warnings, "quote-share"));
	g_assert_true(warned(warnings, "suid-extra"));
	g_assert_true(warned(warnings, "space-extra"));
	g_assert_true(warned(warnings, "option-server"));
	g_assert_true(warned(warnings, "unknown type"));
	g_assert_true(warned(warnings, "dotdot"));
	g_assert_true(warned(warnings, "newline-user"));
	g_assert_true(warned(warnings, "credentials-extra"));
	g_assert_true(warned(warnings, "bad-bool"));
	g_assert_true(warned(warnings, "used twice"));
	bar_harness_down(&h);
}

/* Catches: the extra-option filter accepting something that undoes the
   widget's own safety options, or splitting on commas wrongly. */
static void
test_option_filter(void)
{
	BarHarness h;
	static const struct { const gchar *opt; gboolean ok; } table[] = {
		{ "vers=3.1.1",           TRUE  },
		{ "rsize=1048576,wsize=1048576", TRUE },
		{ "cache=none",           TRUE  },
		{ "nfsvers=4.2",          TRUE  },
		{ "suid",                 FALSE },
		{ "rw,suid",              FALSE },
		{ "SUID",                 FALSE },
		{ "exec",                 FALSE },
		{ "dev",                  FALSE },
		{ "uid=0",                FALSE },
		{ "password=x",           FALSE },
		{ "credentials=x",        FALSE },
		{ "allow_other",          FALSE },
		{ "ssh_command=evil",     FALSE },
		{ "a b",                  FALSE },
		{ "a;b",                  FALSE },
		{ "$(id)",                FALSE },
		{ "-o",                   FALSE },
		{ "vers=3,,x",            FALSE },
		{ "",                     FALSE },
	};
	guint i;

	if (!model_up(&h))
		return;
	for (i = 0; i < G_N_ELEMENTS(table); i++) {
		gboolean got = api.option_ok(table[i].opt, NULL);

		if (got != table[i].ok)
			g_error("option \"%s\": expected %s", table[i].opt,
			        table[i].ok ? "accepted" : "refused");
	}
	bar_harness_down(&h);
}

/* ── Mount points ─────────────────────────────────────────────────── */

/* Catches: a mount point that escapes the allowed trees by `..', by a
   symlink in the user's own home pointing at /etc, by being the home
   itself or `/', and a privileged/unprivileged mix-up (a /mnt path
   created without root, a home path asking for a polkit prompt). */
static void
test_mountpoint_validation(void)
{
	BarHarness h;
	g_autofree gchar *home = NULL;
	g_autofree gchar *real = NULL;
	g_autofree gchar *link = NULL;
	g_autofree gchar *escape = NULL;
	g_autofree gchar *file = NULL;
	g_autofree gchar *canon_home = NULL;
	gchar *canonical = NULL;
	gboolean privileged = FALSE;
	static const gchar *const refused[] = {
		"", "/", "~", "~/", "/etc/x", "/mnt", "/media",
		"~/../x", "~/mnt/../../x", "/mnt/./x", "~/escape/x",
		"~/escape", "~/a-file", "relative/x", NULL
	};
	guint i;

	if (!model_up(&h))
		return;
	home = bar_harness_path(&h, "home");
	g_assert_cmpint(g_mkdir_with_parents(home, 0700), ==, 0);
	{
		gchar *r = realpath(home, NULL);

		canon_home = g_strdup(r);
		free(r);
	}
	real = g_build_filename(home, "real", NULL);
	g_assert_cmpint(g_mkdir(real, 0700), ==, 0);
	link = g_build_filename(home, "inside", NULL);
	g_assert_cmpint(symlink(real, link), ==, 0);
	escape = g_build_filename(home, "escape", NULL);
	g_assert_cmpint(symlink("/etc", escape), ==, 0);
	file = g_build_filename(home, "a-file", NULL);
	g_assert_true(g_file_set_contents(file, "x", -1, NULL));

	for (i = 0; refused[i] != NULL; i++) {
		g_autoptr(GError) error = NULL;

		if (api.resolve(refused[i], home, &canonical, &privileged,
		                &error)) {
			g_error("\"%s\" was accepted as %s", refused[i],
			        canonical);
		}
		g_assert_nonnull(error);
		g_assert_null(canonical);
	}

	/* Inside home, not existing yet: fine, and needs no root. */
	g_assert_true(api.resolve("~/mnt/nas", home, &canonical, &privileged,
	                          NULL));
	g_assert_false(privileged);
	{
		g_autofree gchar *want = g_build_filename(canon_home, "mnt",
		                                          "nas", NULL);

		g_assert_cmpstr(canonical, ==, want);
	}
	g_clear_pointer(&canonical, g_free);

	/* Through a link that stays inside home: resolved to the target. */
	g_assert_true(api.resolve("~/inside/share", home, &canonical,
	                          &privileged, NULL));
	{
		g_autofree gchar *want = g_build_filename(canon_home, "real",
		                                          "share", NULL);

		g_assert_cmpstr(canonical, ==, want);
	}
	g_clear_pointer(&canonical, g_free);

	/* Under /mnt (wherever /mnt really is on this host): root makes
	   it.  A path that cannot exist, so nothing is touched. */
	g_assert_true(api.resolve("/mnt/gowl-netshares-test-none/x", home,
	                          &canonical, &privileged, NULL));
	g_assert_true(privileged);
	g_clear_pointer(&canonical, g_free);
	g_assert_true(api.resolve("/media/gowl-netshares-test-none", home,
	                          &canonical, &privileged, NULL));
	g_assert_true(privileged);
	g_clear_pointer(&canonical, g_free);
	bar_harness_down(&h);
}

/* ── The mount table ─────────────────────────────────────────────── */

/* Catches: a mount point with a space never matching its own entry
   (the \040 escape), the filesystem type read from the wrong field when
   optional fields are present, a malformed line read as a mount, and
   an over-mount not hiding what is under it. */
static void
test_mountinfo(void)
{
	BarHarness h;
	g_autofree gchar *text = NULL;
	g_autoptr(GPtrArray) mounts = NULL;
	const BarNetsharesMount *m;
	guint i;

	if (!model_up(&h))
		return;
	text = fixture_text("mountinfo.txt");
	mounts = api.parse_mountinfo(text);
	/* Eight lines, one without a separator. */
	g_assert_cmpuint(mounts->len, ==, 7);

	m = api.find_mount(mounts, BAR_NETSHARES_CIFS,
	                   "/var/home/zach/mnt/nas media");
	g_assert_nonnull(m);
	g_assert_cmpstr(m->source, ==, "//nas.lan/media");

	/* nfs4 under an NFS connection; optional `master:7' skipped. */
	m = api.find_mount(mounts, BAR_NETSHARES_NFS,
	                   "/var/home/zach/mnt/backup");
	g_assert_nonnull(m);
	g_assert_cmpstr(m->fstype, ==, "nfs4");

	g_assert_nonnull(api.find_mount(mounts, BAR_NETSHARES_SSHFS,
	                                "/var/home/zach/mnt/devbox"));
	/* The right path with the wrong type is not "mounted". */
	g_assert_null(api.find_mount(mounts, BAR_NETSHARES_CIFS,
	                             "/var/home/zach/mnt/devbox"));

	for (i = 0; i < mounts->len; i++) {
		m = g_ptr_array_index(mounts, i);
		if (strstr(m->mountpoint, "tab") != NULL)
			g_assert_cmpstr(m->mountpoint, ==,
			                "/var/home/zach/mnt/tab\tand\\slash");
	}

	/* A tmpfs mounted over the NFS share hides it. */
	g_assert_null(api.find_mount(mounts, BAR_NETSHARES_NFS,
	                             "/var/home/zach/mnt/stacked"));
	bar_harness_down(&h);
}

/* ── Command lines ────────────────────────────────────────────────── */

static BarNetshare *
make_share(BarNetsharesType type, const gchar *server, const gchar *share,
           const gchar *user)
{
	BarNetshare *s = api.share_new();

	s->id = g_strdup("t");
	s->name = g_strdup("T");
	s->type = type;
	s->server = g_strdup(server);
	s->share = g_strdup(share);
	s->user = g_strdup(user);
	s->mountpoint = g_strdup("~/mnt/t");
	return s;
}

/* Catches: any drift in what is handed to a root mount -- a missing
   `seal', `nosuid' or `noexec', uid/gid not mapped to the user, the
   password showing up anywhere in argv -- and the mkdir wrapper putting
   a value into the script text instead of a positional parameter. */
static void
test_argv_cifs(void)
{
	BarHarness h;
	BarNetsharesTools tools = { "pkexec", "sshfs", "fusermount3" };
	BarNetshare *s;
	g_auto(GStrv) argv = NULL;
	g_auto(GStrv) wrap = NULL;
	g_auto(GStrv) guest = NULL;
	static const gchar *const want[] = {
		"pkexec", "mount", "-t", "cifs", "//nas.lan/Shared Music",
		"/home/u/mnt/t", "-o",
		"credentials=/run/user/1000/gowl-netshares/t.cred,uid=1000,"
		"gid=1000,file_mode=0600,dir_mode=0700,seal,nosuid,nodev,"
		"noexec,rw,port=4450,vers=3.1.1",
		NULL
	};
	guint i;

	if (!model_up(&h))
		return;
	s = make_share(BAR_NETSHARES_CIFS, "nas.lan", "Shared Music", "zach");
	s->port = 4450;
	s->extra = g_strsplit("vers=3.1.1", "|", -1);

	argv = api.mount_argv(s, "/home/u/mnt/t", &tools,
	                      "/run/user/1000/gowl-netshares/t.cred",
	                      1000, 1000, FALSE, NULL);
	assert_argv(argv, want);

	/* With the mount point to create: one pkexec, a constant script,
	   values only as positional parameters. */
	wrap = api.mount_argv(s, "/mnt/it's; rm -rf ~", &tools, NULL,
	                      1000, 1000, TRUE, NULL);
	g_assert_cmpstr(wrap[0], ==, "pkexec");
	g_assert_cmpstr(wrap[1], ==, "/bin/sh");
	g_assert_cmpstr(wrap[2], ==, "-c");
	g_assert_null(strstr(wrap[3], "rm -rf"));
	g_assert_cmpstr(wrap[4], ==, "gowl-netshares");
	g_assert_cmpstr(wrap[5], ==, "/mnt/it's; rm -rf ~");
	g_assert_cmpstr(wrap[6], ==, "1000:1000");
	g_assert_cmpstr(wrap[7], ==, "-t");

	/* No user: a guest mount, no credentials file. */
	g_free(s->user);
	s->user = NULL;
	s->encrypt = FALSE;
	guest = api.mount_argv(s, "/m", &tools, NULL, 1, 2, FALSE, NULL);
	g_assert_true(g_str_has_prefix(guest[7], "guest,uid=1,gid=2,"));
	g_assert_null(strstr(guest[7], "seal"));

	/* Neither argv carries anything shaped like a password. */
	for (i = 0; argv[i] != NULL; i++)
		g_assert_null(strstr(argv[i], "password"));
	api.share_free(s);
	bar_harness_down(&h);
}

/* Catches: an NFS device built without the colon, an IPv6 address not
   bracketed (mount would split it at the first colon), the per-share
   version not overriding the widget default. */
static void
test_argv_nfs(void)
{
	BarHarness h;
	BarNetsharesTools tools = { "/fake/pkexec", "sshfs", "fusermount3" };
	BarNetshare *s;
	g_auto(GStrv) argv = NULL;
	g_auto(GStrv) v6 = NULL;
	static const gchar *const want[] = {
		"/fake/pkexec", "mount", "-t", "nfs", "192.168.1.20:/srv/backup",
		"/m", "-o", "nosuid,nodev,vers=4.1,noexec,ro", NULL
	};

	if (!model_up(&h))
		return;
	s = make_share(BAR_NETSHARES_NFS, "192.168.1.20", "/srv/backup", NULL);
	s->readonly = TRUE;
	argv = api.mount_argv(s, "/m", &tools, NULL, 1, 1, FALSE, "4.1");
	assert_argv(argv, want);

	g_free(s->server);
	s->server = g_strdup("fd00::20");
	s->nfs_version = g_strdup("4.2");
	v6 = api.mount_argv(s, "/m", &tools, NULL, 1, 1, FALSE, "3");
	g_assert_cmpstr(v6[4], ==, "[fd00::20]:/srv/backup");
	g_assert_nonnull(strstr(v6[7], "vers=4.2"));
	g_assert_null(strstr(v6[7], "vers=3"));
	api.share_free(s);
	bar_harness_down(&h);
}

/* Catches: SSHFS run through pkexec (it must be the user's own FUSE
   mount), BatchMode missing (a password prompt nobody sees until the
   timeout), the port dropped. */
static void
test_argv_sshfs(void)
{
	BarHarness h;
	BarNetsharesTools tools = { "pkexec", "/fake/sshfs", "fusermount3" };
	BarNetshare *s;
	g_auto(GStrv) argv = NULL;
	g_auto(GStrv) home = NULL;
	static const gchar *const want[] = {
		"/fake/sshfs", "zach@devbox.lan:/home/zach/src", "/m", "-o",
		"reconnect,ServerAliveInterval=15,BatchMode=yes,idmap=user,"
		"noexec,rw,port=2222", NULL
	};

	if (!model_up(&h))
		return;
	s = make_share(BAR_NETSHARES_SSHFS, "devbox.lan", "/home/zach/src",
	               "zach");
	s->port = 2222;
	argv = api.mount_argv(s, "/m", &tools, NULL, 1, 1, FALSE, NULL);
	assert_argv(argv, want);

	g_free(s->share);
	s->share = g_strdup("");
	g_free(s->user);
	s->user = NULL;
	home = api.mount_argv(s, "/m", &tools, NULL, 1, 1, FALSE, NULL);
	g_assert_cmpstr(home[1], ==, "devbox.lan:");
	api.share_free(s);
	bar_harness_down(&h);
}

/* Catches: SSHFS unmounted through pkexec, the lazy flag missing, and
   the secret-tool command putting anything but the id in argv. */
static void
test_argv_unmount_and_secret(void)
{
	BarHarness h;
	BarNetsharesTools tools = { "pkexec", "sshfs", "fusermount3" };
	BarNetshare *cifs, *ssh;
	g_auto(GStrv) a = NULL;
	g_auto(GStrv) b = NULL;
	g_auto(GStrv) c = NULL;
	g_auto(GStrv) d = NULL;
	g_auto(GStrv) store = NULL;
	static const gchar *const want_a[] = { "pkexec", "umount", "/m", NULL };
	static const gchar *const want_b[] = { "pkexec", "umount", "-l", "/m",
	                                       NULL };
	static const gchar *const want_c[] = { "fusermount3", "-u", "/m", NULL };
	static const gchar *const want_d[] = { "fusermount3", "-uz", "/m", NULL };
	static const gchar *const want_s[] = {
		"secret-tool", "store", "--label=gowl netshares NAS media",
		"service", "gowl-netshares", "id", "nas-media", NULL
	};

	if (!model_up(&h))
		return;
	cifs = make_share(BAR_NETSHARES_CIFS, "nas", "x", NULL);
	ssh  = make_share(BAR_NETSHARES_SSHFS, "nas", "", NULL);
	a = api.unmount_argv(cifs, "/m", &tools, FALSE);
	b = api.unmount_argv(cifs, "/m", &tools, TRUE);
	c = api.unmount_argv(ssh, "/m", &tools, FALSE);
	d = api.unmount_argv(ssh, "/m", &tools, TRUE);
	assert_argv(a, want_a);
	assert_argv(b, want_b);
	assert_argv(c, want_c);
	assert_argv(d, want_d);
	store = api.secret_argv("secret-tool", "store", "nas-media",
	                        "NAS media");
	assert_argv(store, want_s);
	api.share_free(cifs);
	api.share_free(ssh);
	bar_harness_down(&h);
}

/* Catches: a domain not split out of DOMAIN\user, a newline that would
   add a second key, a credentials file created with group or world
   bits, and a symlink planted at the file's name being written
   through. */
static void
test_credentials(void)
{
	BarHarness h;
	g_autofree gchar *text = NULL;
	g_autofree gchar *dir = NULL;
	g_autofree gchar *path = NULL;
	g_autofree gchar *planted = NULL;
	g_autofree gchar *victim = NULL;
	g_autofree gchar *victim_text = NULL;
	g_autoptr(GError) error = NULL;
	GStatBuf st;

	if (!model_up(&h))
		return;
	text = api.cred_text("CORP\\zach", "p@ss word", NULL);
	g_assert_cmpstr(text, ==,
	                "username=zach\npassword=p@ss word\ndomain=CORP\n");
	g_assert_null(api.cred_text("zach", "a\nusername=root", &error));
	g_assert_nonnull(error);
	g_clear_error(&error);

	dir = bar_harness_path(&h, "rt/gowl-netshares");
	g_assert_cmpint(g_mkdir_with_parents(dir, 0755), ==, 0);
	victim = bar_harness_path(&h, "victim");
	g_assert_true(g_file_set_contents(victim, "untouched", -1, NULL));
	planted = g_build_filename(dir, "nas.cred", NULL);
	g_assert_cmpint(symlink(victim, planted), ==, 0);

	path = api.write_cred(dir, "nas", "zach", "hunter2", &error);
	g_assert_no_error(error);
	g_assert_cmpstr(path, ==, planted);
	g_assert_cmpint(g_lstat(path, &st), ==, 0);
	g_assert_true(S_ISREG(st.st_mode));
	g_assert_cmpint(st.st_mode & 0777, ==, 0600);
	g_assert_cmpint(g_stat(dir, &st), ==, 0);
	g_assert_cmpint(st.st_mode & 0777, ==, 0700);
	victim_text = read_or_empty(victim);
	g_assert_cmpstr(victim_text, ==, "untouched");

	/* An id that could name another path is refused. */
	g_assert_null(api.write_cred(dir, "../x", "zach", "p", NULL));
	bar_harness_down(&h);
}

/* ── Running ──────────────────────────────────────────────────────── */

/* Catches: a hung helper holding the worker forever, the kill reaching
   only the first process and not the group, and a deadline that is
   not honoured. */
static void
test_run_timeout(void)
{
	BarHarness h;
	g_autofree gchar *script = NULL;
	g_autofree gchar *pidfile = NULL;
	g_autofree gchar *pidtext = NULL;
	const gchar *argv[2];
	BarNetsharesRun run;
	gint64 t0;
	pid_t child;

	if (!model_up(&h))
		return;
	pidfile = bar_harness_path(&h, "sleeper.pid");
	{
		g_autofree gchar *body = g_strdup_printf(
			"sleep 30 & echo $! > '%s'; wait", pidfile);

		script = bar_harness_script(&h, "sleeper", body);
	}
	argv[0] = script;
	argv[1] = NULL;

	t0 = g_get_monotonic_time();
	g_assert_true(api.run(argv, NULL, 1, &run, NULL));
	g_assert_true(run.timed_out);
	g_assert_cmpint(run.status, ==, -1);
	g_assert_cmpint(g_get_monotonic_time() - t0, <, 4 * G_USEC_PER_SEC);
	api.run_clear(&run);

	/* The grandchild went with the group. */
	pidtext = read_or_empty(pidfile);
	child = (pid_t)atoi(pidtext);
	g_assert_cmpint(child, >, 0);
	{
		gint tries;

		for (tries = 0; tries < 40 && kill(child, 0) == 0; tries++)
			g_usleep(25 * 1000);
	}
	g_assert_cmpint(kill(child, 0), ==, -1);
	bar_harness_down(&h);
}

/* Catches: SIGPIPE killing the process when a child exits without
   reading its stdin, and a child that never reads blocking the write.
   If the first half regresses, this test binary dies with the signal. */
static void
test_run_stdin_hazards(void)
{
	BarHarness h;
	const gchar *quit[] = { "/bin/sh", "-c", "exit 3", NULL };
	const gchar *stall[] = { "/bin/sh", "-c", "sleep 30", NULL };
	const gchar *cat[] = { "/bin/cat", NULL };
	g_autofree gchar *big = NULL;
	BarNetsharesRun run;
	gint64 t0;

	if (!model_up(&h))
		return;
	big = g_strnfill(1024 * 1024, 'x');

	g_assert_true(api.run(quit, big, 5, &run, NULL));
	g_assert_cmpint(run.status, ==, 3);
	api.run_clear(&run);

	t0 = g_get_monotonic_time();
	g_assert_true(api.run(stall, big, 1, &run, NULL));
	g_assert_true(run.timed_out);
	g_assert_cmpint(g_get_monotonic_time() - t0, <, 5 * G_USEC_PER_SEC);
	api.run_clear(&run);

	/* And the ordinary case: stdin reaches the child. */
	g_assert_true(api.run(cat, "hello", 5, &run, NULL));
	g_assert_cmpstr(run.out, ==, "hello");
	api.run_clear(&run);
	bar_harness_down(&h);
}

/* Catches: helper output reaching pango unvalidated, an escape
   sequence painted into the panel, a password echoed by a helper being
   shown, and a novel in one subtitle. */
static void
test_last_line(void)
{
	BarHarness h;
	g_autofree gchar *a = NULL;
	g_autofree gchar *b = NULL;
	g_autofree gchar *c = NULL;
	g_autofree gchar *long_text = NULL;
	g_autofree gchar *d = NULL;

	if (!model_up(&h))
		return;
	a = api.last_line("first\nmount error(13): Permission denied\n\n",
	                  NULL);
	g_assert_cmpstr(a, ==, "mount error(13): Permission denied");
	b = api.last_line("bad \xff byte \x1b[31mred\x1b[0m", NULL);
	g_assert_true(g_utf8_validate(b, -1, NULL));
	g_assert_null(strchr(b, '\x1b'));
	c = api.last_line("helper said hunter2 out loud", "hunter2");
	g_assert_null(strstr(c, "hunter2"));
	g_assert_nonnull(strstr(c, "***"));
	long_text = g_strnfill(5000, 'z');
	d = api.last_line(long_text, NULL);
	g_assert_cmpint(g_utf8_strlen(d, -1), <=, 210);
	g_assert_null(api.last_line("\n  \n", NULL));
	g_assert_true(api.is_busy("umount: /m: target is busy."));
	g_assert_false(api.is_busy("umount: /m: not mounted."));
	g_assert_cmpstr(api.package_for("mount.cifs"), ==, "cifs-utils");
	g_assert_cmpstr(api.package_for("/usr/bin/sshfs"), ==, "fuse-sshfs");
	g_assert_cmpstr(api.package_for("mount.nfs"), ==, "nfs-utils");
	g_assert_cmpstr(api.package_for("fusermount3"), ==, "fuse3");
	g_assert_cmpstr(api.package_for("pkexec"), ==, "polkit");
	g_assert_cmpstr(api.package_for("secret-tool"), ==, "libsecret");
	bar_harness_down(&h);
}

/* ── The widget, end to end ───────────────────────────────────────── */

/*
 * A scratch world: a home, a runtime directory, a fake mount table,
 * stand-ins for every helper, and a log directory they write into.
 *
 * The stand-in pkexec does what mount would do to the table: a mount
 * appends a line, an umount removes it.  Its behaviour comes from
 * $LOG/pkexec.mode: ok (default), fail, busy (umount without -l is
 * refused), sleep.
 */
typedef struct {
	BarHarness  h;
	gchar      *home;
	gchar      *canon_home;
	gchar      *log;
	gchar      *mi;
	gchar      *config;
	gchar      *bin;
} Rig;

static gchar *
subst(const gchar *body, const gchar *log, const gchar *mi)
{
	g_autofree gchar *a = NULL;
	GRegex *re;
	gchar *out;

	re = g_regex_new("@LOG@", 0, 0, NULL);
	a = g_regex_replace_literal(re, body, -1, 0, log, 0, NULL);
	g_regex_unref(re);
	re = g_regex_new("@MI@", 0, 0, NULL);
	out = g_regex_replace_literal(re, a, -1, 0, mi, 0, NULL);
	g_regex_unref(re);
	return out;
}

static const gchar fake_pkexec[] =
	"LOG='@LOG@'; MI='@MI@'\n"
	"mode=$(cat \"$LOG/pkexec.mode\" 2>/dev/null || echo ok)\n"
	"{ printf '%s\\n' \"$@\"; echo ---; } >> \"$LOG/pkexec.argv\"\n"
	"env >> \"$LOG/pkexec.env\"\n"
	"for a in \"$@\"; do case \"$a\" in *credentials=*)\n"
	"  c=${a#*credentials=}; c=${c%%,*}\n"
	"  stat -c %a \"$c\" > \"$LOG/cred.mode\"\n"
	"  grep -c '^password=' \"$c\" > \"$LOG/cred.haspw\"\n"
	"  echo \"$c\" > \"$LOG/cred.path\";;\n"
	"esac; done\n"
	"case \"$mode\" in\n"
	"  sleep) sleep 30; exit 0;;\n"
	"  fail) echo 'mount error(13): Permission denied' >&2;\n"
	"        echo 'Refer to the mount.cifs(8) manual page' >&2; exit 32;;\n"
	"  busy) if [ \"$1\" = umount ] && [ \"$2\" != -l ]; then\n"
	"          echo \"umount: $2: target is busy.\" >&2; exit 32; fi;;\n"
	"esac\n"
	"if [ \"$1\" = umount ]; then\n"
	"  mp=\"${!#}\"; grep -v \" $mp \" \"$MI\" > \"$MI.tmp\";"
	" mv \"$MI.tmp\" \"$MI\"; exit 0\n"
	"fi\n"
	"args=(\"$@\")\n"
	"for i in \"${!args[@]}\"; do if [ \"${args[$i]}\" = -t ]; then\n"
	"  t=${args[$((i+1))]}; dev=${args[$((i+2))]}; mp=${args[$((i+3))]}\n"
	"fi; done\n"
	"[ \"$t\" = nfs ] && t=nfs4\n"
	"echo \"200 22 0:99 / $mp rw - $t $dev rw\" >> \"$MI\"\n"
	"exit 0\n";

static const gchar fake_secret[] =
	"LOG='@LOG@'\n"
	"{ printf '%s\\n' \"$@\"; echo ---; } >> \"$LOG/secret.argv\"\n"
	"env >> \"$LOG/secret.env\"\n"
	"case \"$1\" in\n"
	"  lookup) [ -f \"$LOG/secret.value\" ] || exit 1;"
	" cat \"$LOG/secret.value\";;\n"
	"  store) cat > \"$LOG/secret.stdin\";;\n"
	"  clear) exit 0;;\n"
	"esac\n";

static const gchar fake_sshfs[] =
	"LOG='@LOG@'; MI='@MI@'\n"
	"{ printf '%s\\n' \"$@\"; echo ---; } >> \"$LOG/sshfs.argv\"\n"
	"echo \"201 22 0:98 / $2 rw - fuse.sshfs $1 rw\" >> \"$MI\"\n"
	"exit 0\n";

static const gchar fake_fusermount[] =
	"LOG='@LOG@'; MI='@MI@'\n"
	"{ printf '%s\\n' \"$@\"; echo ---; } >> \"$LOG/fusermount.argv\"\n"
	"mp=\"${!#}\"; grep -v \" $mp \" \"$MI\" > \"$MI.tmp\";"
	" mv \"$MI.tmp\" \"$MI\"\n";

static gchar *
rig_script(Rig *r, const gchar *name, const gchar *body)
{
	g_autofree gchar *text = subst(body, r->log, r->mi);

	return bar_harness_script(&r->h, name, text);
}

static gboolean
rig_up(Rig *r)
{
	g_autofree gchar *rt = NULL;
	g_autofree gchar *pk = NULL;
	g_autofree gchar *st = NULL;
	g_autofree gchar *ss = NULL;
	g_autofree gchar *fm = NULL;
	g_autofree gchar *helper = NULL;
	gchar *real;

	memset(r, 0, sizeof(*r));
	if (!bar_harness_up(&r->h, "bar_register_netshares_plugins",
	                    "netshares"))
		return FALSE;
	if (!api_up(&r->h))
		return FALSE;

	r->home = bar_harness_path(&r->h, "home");
	r->log  = bar_harness_path(&r->h, "log");
	r->mi   = bar_harness_path(&r->h, "mountinfo");
	r->bin  = bar_harness_path(&r->h, "bin");
	r->config = bar_harness_path(&r->h, "home/.config/gowl/netshares.yaml");
	rt = bar_harness_path(&r->h, "rt");
	g_mkdir_with_parents(r->home, 0700);
	g_mkdir_with_parents(r->log, 0700);
	g_mkdir_with_parents(r->bin, 0700);
	g_mkdir_with_parents(rt, 0700);
	real = realpath(r->home, NULL);
	r->canon_home = g_strdup(real);
	free(real);
	g_assert_true(g_file_set_contents(r->mi,
		"22 1 0:21 / / rw - btrfs /dev/x rw\n", -1, NULL));

	/* The widget reads these at call time, not at startup. */
	g_setenv("HOME", r->home, TRUE);
	g_setenv("XDG_RUNTIME_DIR", rt, TRUE);

	pk = rig_script(r, "pkexec", fake_pkexec);
	st = rig_script(r, "secret-tool", fake_secret);
	ss = rig_script(r, "sshfs", fake_sshfs);
	fm = rig_script(r, "fusermount3", fake_fusermount);
	/* The mount helpers only need to EXIST, for the installed check. */
	helper = g_build_filename(r->bin, "mount.cifs", NULL);
	g_assert_true(g_file_set_contents(helper, "#!/bin/sh\n", -1, NULL));
	g_chmod(helper, 0700);
	g_free(helper);
	helper = g_build_filename(r->bin, "mount.nfs", NULL);
	g_assert_true(g_file_set_contents(helper, "#!/bin/sh\n", -1, NULL));
	g_chmod(helper, 0700);

	gowl_bar_plugin_set_setting(r->h.plugin, "config", r->config);
	gowl_bar_plugin_set_setting(r->h.plugin, "mountinfo", r->mi);
	gowl_bar_plugin_set_setting(r->h.plugin, "pkexec", pk);
	gowl_bar_plugin_set_setting(r->h.plugin, "secret-tool", st);
	gowl_bar_plugin_set_setting(r->h.plugin, "sshfs", ss);
	gowl_bar_plugin_set_setting(r->h.plugin, "fusermount", fm);
	gowl_bar_plugin_set_setting(r->h.plugin, "helper-path", r->bin);
	gowl_bar_plugin_set_setting(r->h.plugin, "timeout", "5");
	gowl_bar_plugin_set_setting(r->h.plugin, "open-command",
	                            "my-files --new");
	gowl_bar_plugin_configure(r->h.plugin, NULL);
	return TRUE;
}

static void
rig_down(Rig *r)
{
	bar_harness_down(&r->h);
	g_free(r->home);
	g_free(r->canon_home);
	g_free(r->log);
	g_free(r->mi);
	g_free(r->config);
	g_free(r->bin);
}

static void
rig_config(Rig *r, const gchar *yaml)
{
	g_autofree gchar *dir = g_path_get_dirname(r->config);

	g_mkdir_with_parents(dir, 0700);
	g_assert_true(g_file_set_contents(r->config, yaml, -1, NULL));
}

static void
rig_mode(Rig *r, const gchar *mode)
{
	g_autofree gchar *path = g_build_filename(r->log, "pkexec.mode", NULL);

	g_assert_true(g_file_set_contents(path, mode, -1, NULL));
}

static gchar *
rig_log(Rig *r, const gchar *name)
{
	g_autofree gchar *path = g_build_filename(r->log, name, NULL);
	gchar *text = read_or_empty(path);

	return g_strstrip(text);
}

/* The canonical path of ~/mnt/<leaf> in the rig's home. */
static gchar *
rig_mp(Rig *r, const gchar *leaf)
{
	return g_build_filename(r->canon_home, "mnt", leaf, NULL);
}

static void
rig_mounted_line(Rig *r, const gchar *fstype, const gchar *leaf)
{
	g_autofree gchar *mp = rig_mp(r, leaf);
	g_autofree gchar *old = read_or_empty(r->mi);
	g_autofree gchar *text = g_strdup_printf("%s300 22 0:1 / %s rw - %s x rw\n",
	                                         old, mp, fstype);

	g_mkdir_with_parents(mp, 0700);
	g_assert_true(g_file_set_contents(r->mi, text, -1, NULL));
}

static const gchar cifs_yaml[] =
	"connections:\n"
	"  - id: nas\n"
	"    name: NAS\n"
	"    type: cifs\n"
	"    server: nas.lan\n"
	"    share: media\n"
	"    user: zach\n"
	"    mountpoint: ~/mnt/nas\n";

/* Catches the whole credential path regressing: the password in argv
   or the environment, a credentials file readable by anyone but the
   user, or one left behind after mount returns.  The stand-in pkexec
   stats the file while it runs, which is the only moment it exists. */
static void
test_mount_cifs_keeps_the_password_out_of_sight(void)
{
	Rig r;
	g_autofree gchar *argv_log = NULL;
	g_autofree gchar *env_log = NULL;
	g_autofree gchar *secret_log = NULL;
	g_autofree gchar *mode = NULL;
	g_autofree gchar *haspw = NULL;
	g_autofree gchar *cred = NULL;
	g_autofree gchar *mp = NULL;
	g_autofree gchar *label = NULL;
	g_autofree gchar *tip = NULL;
	g_autofree gchar *value = NULL;
	g_autoptr(GowlBarPanel) panel = NULL;

	if (!rig_up(&r))
		return;
	rig_config(&r, cifs_yaml);
	value = g_build_filename(r.log, "secret.value", NULL);
	g_assert_true(g_file_set_contents(value, "hunter2", -1, NULL));

	bar_harness_poll(&r.h);
	label = gowl_bar_plugin_dup_label(r.h.plugin);
	g_assert_null(label);
	g_assert_cmpint(gowl_bar_plugin_get_color(r.h.plugin), ==,
	                GOWL_BAR_COLOR_SUBTEXT);

	gowl_bar_plugin_panel_action(r.h.plugin, "toggle:nas", -1, 0, 1);

	argv_log   = rig_log(&r, "pkexec.argv");
	env_log    = rig_log(&r, "pkexec.env");
	secret_log = rig_log(&r, "secret.argv");
	mode       = rig_log(&r, "cred.mode");
	haspw      = rig_log(&r, "cred.haspw");
	cred       = rig_log(&r, "cred.path");
	mp         = rig_mp(&r, "nas");

	g_test_message("pkexec argv:\n%s", argv_log);
	g_assert_nonnull(strstr(argv_log, "mount\n-t\ncifs\n//nas.lan/media\n"));
	g_assert_nonnull(strstr(argv_log, mp));
	g_assert_null(strstr(argv_log, "hunter2"));
	g_assert_null(strstr(env_log, "hunter2"));
	g_assert_null(strstr(secret_log, "hunter2"));
	g_assert_nonnull(strstr(secret_log, "lookup\nservice\ngowl-netshares\n"
	                                    "id\nnas"));
	g_assert_cmpstr(mode, ==, "600");
	g_assert_cmpstr(haspw, ==, "1");
	g_assert_nonnull(strstr(cred, "gowl-netshares/nas.cred"));
	g_assert_false(g_file_test(cred, G_FILE_TEST_EXISTS));
	/* The mount point was made, as the user, 0700. */
	g_assert_true(g_file_test(mp, G_FILE_TEST_IS_DIR));

	/* The table now says mounted; so do the bar and the panel. */
	gowl_bar_plugin_poll(r.h.plugin);
	g_free(label);
	label = gowl_bar_plugin_dup_label(r.h.plugin);
	g_assert_cmpstr(label, ==, "1");
	g_assert_cmpint(gowl_bar_plugin_get_color(r.h.plugin), ==,
	                GOWL_BAR_COLOR_GREEN);
	tip = gowl_bar_plugin_dup_tooltip(r.h.plugin);
	g_assert_nonnull(strstr(tip, "1 of 1 mounted"));
	g_assert_null(strstr(tip, "hunter2"));
	g_assert_true(bar_harness_toasted(&r.h, "Mounted NAS"));
	panel = bar_harness_panel(&r.h);
	g_assert_true(bar_harness_panel_mentions(panel, "mounted"));
	g_assert_true(bar_harness_panel_mentions(panel, "1 of 1 mounted"));

	/* Open hands the mount point to the file manager, quoted. */
	gowl_bar_plugin_panel_action(r.h.plugin, "open:nas", -1, 0, 1);
	g_assert_true(bar_harness_spawned(&r.h, "my-files --new '"));
	g_assert_true(bar_harness_spawned(&r.h, mp));

	/* And unmount takes it back out of the table. */
	gowl_bar_plugin_panel_action(r.h.plugin, "toggle:nas", -1, 0, 1);
	gowl_bar_plugin_poll(r.h.plugin);
	g_free(label);
	label = gowl_bar_plugin_dup_label(r.h.plugin);
	g_assert_null(label);
	g_assert_true(bar_harness_toasted(&r.h, "Unmounted NAS"));
	rig_down(&r);
}

/* Catches: a mount failure that is silent, or whose message is the
   helper's usage boilerplate instead of the reason; and a missing
   keyring entry that fails without asking for the password. */
static void
test_mount_failures_say_why(void)
{
	Rig r;
	g_autoptr(GowlBarPanel) panel = NULL;
	g_autoptr(GowlBarPanel) panel2 = NULL;
	g_autofree gchar *value = NULL;

	if (!rig_up(&r))
		return;
	rig_config(&r, cifs_yaml);
	bar_harness_poll(&r.h);

	/* No password stored: say so and open the password field. */
	gowl_bar_plugin_panel_action(r.h.plugin, "toggle:nas", -1, 0, 1);
	gowl_bar_plugin_poll(r.h.plugin);
	g_assert_true(bar_harness_toasted(&r.h, "No password is stored"));
	panel = bar_harness_panel(&r.h);
	g_assert_nonnull(gowl_bar_panel_find(panel, "password-entry"));
	g_assert_true(bar_harness_panel_mentions(panel, "Password for NAS"));

	/* The helper refuses: the reason, not the man-page pointer. */
	value = g_build_filename(r.log, "secret.value", NULL);
	g_assert_true(g_file_set_contents(value, "pw", -1, NULL));
	rig_mode(&r, "fail");
	gowl_bar_plugin_panel_action(r.h.plugin, "toggle:nas", -1, 0, 1);
	gowl_bar_plugin_poll(r.h.plugin);
	panel2 = bar_harness_panel(&r.h);
	g_assert_true(bar_harness_panel_mentions(panel2,
		"mount error(13): Permission denied"));
	g_assert_false(bar_harness_panel_mentions(panel2, "Refer to"));
	g_assert_true(bar_harness_toasted(&r.h, "Could not mount NAS"));
	g_assert_cmpint(gowl_bar_plugin_get_color(r.h.plugin), ==,
	                GOWL_BAR_COLOR_YELLOW);
	rig_down(&r);
}

/* Catches: a hung pkexec (no polkit agent, a dead server) holding the
   row busy forever, and the credentials file outliving the attempt. */
static void
test_mount_timeout(void)
{
	Rig r;
	g_autofree gchar *value = NULL;
	g_autofree gchar *cred = NULL;
	g_autoptr(GowlBarPanel) panel = NULL;
	gint64 t0;

	if (!rig_up(&r))
		return;
	rig_config(&r, cifs_yaml);
	value = g_build_filename(r.log, "secret.value", NULL);
	g_assert_true(g_file_set_contents(value, "pw", -1, NULL));
	bar_harness_set(&r.h, "timeout", "1");
	bar_harness_poll(&r.h);
	rig_mode(&r, "sleep");

	t0 = g_get_monotonic_time();
	gowl_bar_plugin_panel_action(r.h.plugin, "toggle:nas", -1, 0, 1);
	g_assert_cmpint(g_get_monotonic_time() - t0, <, 5 * G_USEC_PER_SEC);
	gowl_bar_plugin_poll(r.h.plugin);
	g_assert_true(bar_harness_toasted(&r.h, "timed out after 1 s"));
	cred = rig_log(&r, "cred.path");
	g_assert_false(g_file_test(cred, G_FILE_TEST_EXISTS));
	panel = bar_harness_panel(&r.h);
	g_assert_true(bar_harness_panel_mentions(panel, "not mounted"));
	rig_down(&r);
}

/* Catches: the password reaching secret-tool through argv rather than
   stdin, and the typed password being kept after it is stored. */
static void
test_store_password_on_stdin(void)
{
	Rig r;
	g_autofree gchar *stdin_log = NULL;
	g_autofree gchar *argv_log = NULL;
	g_autoptr(GowlBarPanel) panel = NULL;
	GowlBarPanelItem *entry;

	if (!rig_up(&r))
		return;
	rig_config(&r, cifs_yaml);
	bar_harness_poll(&r.h);

	gowl_bar_plugin_panel_action(r.h.plugin, "password:nas", -1, 0, 1);
	panel = bar_harness_panel(&r.h);
	entry = gowl_bar_panel_find(panel, "password-entry");
	g_assert_nonnull(entry);
	g_assert_cmpstr(gowl_bar_panel_item_get_prop(entry,
		GOWL_BAR_ENTRY_PROP_SECRET), ==, "true");

	gowl_bar_plugin_panel_text(r.h.plugin, "password-entry", "s3cr3t pa$$",
	                           FALSE);
	gowl_bar_plugin_panel_text(r.h.plugin, "password-entry", "s3cr3t pa$$",
	                           TRUE);
	stdin_log = rig_log(&r, "secret.stdin");
	argv_log  = rig_log(&r, "secret.argv");
	g_assert_cmpstr(stdin_log, ==, "s3cr3t pa$$");
	g_assert_null(strstr(argv_log, "s3cr3t"));
	g_assert_nonnull(strstr(argv_log,
		"store\n--label=gowl netshares NAS\nservice\ngowl-netshares\n"
		"id\nnas"));
	gowl_bar_plugin_poll(r.h.plugin);
	g_assert_true(bar_harness_toasted(&r.h, "Password saved for NAS"));
	g_assert_false(bar_harness_toasted(&r.h, "s3cr3t"));
	rig_down(&r);
}

/* Catches: an unmount with open files that only fails, with no way
   forward; a force (lazy) unmount that fires on one click; and the
   lazy flag missing from the second attempt. */
static void
test_unmount_busy_offers_lazy(void)
{
	Rig r;
	g_autoptr(GowlBarPanel) p1 = NULL;
	g_autoptr(GowlBarPanel) p2 = NULL;
	g_autoptr(GowlBarPanel) p3 = NULL;
	GowlBarPanelItem *row;
	g_autofree gchar *argv_log = NULL;
	gint force = -1;
	guint i;

	if (!rig_up(&r))
		return;
	rig_config(&r, cifs_yaml);
	rig_mounted_line(&r, "cifs", "nas");
	bar_harness_poll(&r.h);
	rig_mode(&r, "busy");

	gowl_bar_plugin_panel_action(r.h.plugin, "toggle:nas", -1, 0, 1);
	gowl_bar_plugin_poll(r.h.plugin);
	g_assert_true(bar_harness_toasted(&r.h, "busy"));

	p1 = bar_harness_panel(&r.h);
	row = gowl_bar_panel_find(p1, "act:nas");
	g_assert_nonnull(row);
	for (i = 0; i < gowl_bar_panel_item_n_children(row); i++)
		if (g_strcmp0(gowl_bar_panel_item_get_title(
			gowl_bar_panel_item_get_child(row, i)),
			"Force unmount") == 0)
			force = (gint)i;
	g_assert_cmpint(force, >=, 0);

	/* First click arms. */
	gowl_bar_plugin_panel_action(r.h.plugin, "act:nas", force, 0, 1);
	p2 = bar_harness_panel(&r.h);
	g_assert_true(bar_harness_panel_mentions(p2, "Click again to force"));
	argv_log = rig_log(&r, "pkexec.argv");
	g_assert_null(strstr(argv_log, "-l"));

	/* Second fires, lazily. */
	gowl_bar_plugin_panel_action(r.h.plugin, "act:nas", force, 0, 1);
	g_free(argv_log);
	argv_log = rig_log(&r, "pkexec.argv");
	g_assert_nonnull(strstr(argv_log, "umount\n-l\n"));
	p3 = bar_harness_panel(&r.h);
	g_assert_true(bar_harness_panel_mentions(p3, "not mounted"));
	g_assert_false(bar_harness_panel_mentions(p3, "Force unmount"));
	rig_down(&r);
}

/* Catches: SSHFS going through pkexec, or its unmount not using the
   FUSE helper. */
static void
test_sshfs_round_trip(void)
{
	Rig r;
	g_autofree gchar *ssh_log = NULL;
	g_autofree gchar *fm_log = NULL;
	g_autofree gchar *pk_log = NULL;
	g_autofree gchar *mp = NULL;
	g_autofree gchar *label = NULL;

	if (!rig_up(&r))
		return;
	rig_config(&r,
		"connections:\n"
		"  - id: dev\n"
		"    name: Dev\n"
		"    type: sshfs\n"
		"    server: devbox.lan\n"
		"    path: /srv\n"
		"    user: zach\n"
		"    mountpoint: ~/mnt/dev\n");
	bar_harness_poll(&r.h);
	mp = rig_mp(&r, "dev");

	gowl_bar_plugin_panel_action(r.h.plugin, "mount:dev", -1, 0, 1);
	ssh_log = rig_log(&r, "sshfs.argv");
	g_assert_nonnull(strstr(ssh_log, "zach@devbox.lan:/srv\n"));
	g_assert_nonnull(strstr(ssh_log, "BatchMode=yes"));
	pk_log = rig_log(&r, "pkexec.argv");
	g_assert_cmpstr(pk_log, ==, "");
	gowl_bar_plugin_poll(r.h.plugin);
	label = gowl_bar_plugin_dup_label(r.h.plugin);
	g_assert_cmpstr(label, ==, "1");

	gowl_bar_plugin_panel_action(r.h.plugin, "unmount:dev", -1, 0, 1);
	fm_log = rig_log(&r, "fusermount.argv");
	g_assert_nonnull(strstr(fm_log, "-u\n"));
	g_assert_nonnull(strstr(fm_log, mp));
	rig_down(&r);
}

/* Catches: forget firing on one click, forgetting a share that is
   still mounted, leaving its keyring entry behind, or rewriting the
   file with loose permissions. */
static void
test_forget(void)
{
	Rig r;
	g_autoptr(GowlBarPanel) p = NULL;
	g_autofree gchar *after = NULL;
	g_autofree gchar *secret_log = NULL;
	GStatBuf st;

	if (!rig_up(&r))
		return;
	rig_config(&r,
		"connections:\n"
		"  - id: nas\n"
		"    name: NAS\n"
		"    type: cifs\n"
		"    server: nas.lan\n"
		"    share: media\n"
		"    mountpoint: ~/mnt/nas\n"
		"  - id: up\n"
		"    name: Up\n"
		"    type: nfs\n"
		"    server: nas.lan\n"
		"    export: /x\n"
		"    mountpoint: ~/mnt/up\n");
	rig_mounted_line(&r, "nfs4", "up");
	bar_harness_poll(&r.h);

	/* Mounted: refused, with a reason. */
	gowl_bar_plugin_panel_action(r.h.plugin, "forget:up", -1, 0, 1);
	gowl_bar_plugin_panel_action(r.h.plugin, "forget:up", -1, 0, 1);
	g_assert_true(bar_harness_toasted(&r.h, "Unmount Up before"));

	gowl_bar_plugin_panel_action(r.h.plugin, "forget:nas", -1, 0, 1);
	p = bar_harness_panel(&r.h);
	g_assert_true(bar_harness_panel_mentions(p, "Click again to forget"));
	after = read_or_empty(r.config);
	g_assert_nonnull(strstr(after, "nas"));

	gowl_bar_plugin_panel_action(r.h.plugin, "forget:nas", -1, 0, 1);
	g_free(after);
	after = read_or_empty(r.config);
	g_assert_null(strstr(after, "\"nas\""));
	g_assert_nonnull(strstr(after, "\"up\""));
	g_assert_cmpint(g_stat(r.config, &st), ==, 0);
	g_assert_cmpint(st.st_mode & 0777, ==, 0600);
	secret_log = rig_log(&r, "secret.argv");
	g_assert_nonnull(strstr(secret_log,
		"clear\nservice\ngowl-netshares\nid\nnas"));
	gowl_bar_plugin_poll(r.h.plugin);
	g_assert_true(bar_harness_toasted(&r.h, "Forgot NAS"));
	rig_down(&r);
}

/* Catches: the add form accepting what the loader would refuse, its
   errors not shown, a save that is not atomic or not 0600, a password
   that lands in the file instead of the keyring, and a symlinked
   connection file (a dotfiles checkout) replaced by a plain file. */
static void
test_add_form(void)
{
	Rig r;
	g_autoptr(GowlBarPanel) p1 = NULL;
	g_autoptr(GowlBarPanel) p2 = NULL;
	g_autoptr(GowlBarPanel) p3 = NULL;
	g_autofree gchar *text = NULL;
	g_autofree gchar *stdin_log = NULL;
	g_autofree gchar *target = NULL;
	g_autoptr(GPtrArray) shares = NULL;
	BarNetshare *s;
	GStatBuf st;

	if (!rig_up(&r))
		return;
	/* The file is a link into a "dotfiles" directory. */
	target = bar_harness_path(&r.h, "dotfiles-netshares.yaml");
	g_assert_true(g_file_set_contents(target, "connections: []\n", -1,
	                                  NULL));
	{
		g_autofree gchar *dir = g_path_get_dirname(r.config);

		g_mkdir_with_parents(dir, 0700);
	}
	g_assert_cmpint(symlink(target, r.config), ==, 0);
	bar_harness_poll(&r.h);

	gowl_bar_plugin_panel_action(r.h.plugin, "add-open", -1, 0, 1);
	p1 = bar_harness_panel(&r.h);
	g_assert_nonnull(gowl_bar_panel_find(p1, "add-name"));
	g_assert_nonnull(gowl_bar_panel_find(p1, "add-password"));

	/* Nothing filled in: an inline error, nothing written. */
	gowl_bar_plugin_panel_action(r.h.plugin, "add-save", 0, 0, 1);
	p2 = bar_harness_panel(&r.h);
	g_assert_true(bar_harness_panel_mentions(p2, "name is required"));

	gowl_bar_plugin_panel_text(r.h.plugin, "add-name", "Office NAS", FALSE);
	gowl_bar_plugin_panel_text(r.h.plugin, "add-server", "-oProxy", FALSE);
	gowl_bar_plugin_panel_text(r.h.plugin, "add-share", "docs", FALSE);
	gowl_bar_plugin_panel_action(r.h.plugin, "add-save", 0, 0, 1);
	g_clear_object(&p2);
	p2 = bar_harness_panel(&r.h);
	g_assert_true(bar_harness_panel_mentions(p2, "server must be"));

	gowl_bar_plugin_panel_text(r.h.plugin, "add-server", "nas.lan", FALSE);
	gowl_bar_plugin_panel_text(r.h.plugin, "add-user", "zach", FALSE);
	gowl_bar_plugin_panel_text(r.h.plugin, "add-password", "pw 1", FALSE);
	/* Return in a field saves, as the Save button does. */
	gowl_bar_plugin_panel_text(r.h.plugin, "add-share", "docs", TRUE);

	text = read_or_empty(target);
	g_assert_null(strstr(text, "pw 1"));
	shares = api.load(text, NULL, NULL);
	g_assert_nonnull(shares);
	s = api.find(shares, "office-nas");
	g_assert_nonnull(s);
	g_assert_cmpstr(s->mountpoint, ==, "~/mnt/office-nas");
	g_assert_true(s->encrypt);
	g_assert_true(s->noexec);
	g_assert_cmpint(g_lstat(r.config, &st), ==, 0);
	g_assert_true(S_ISLNK(st.st_mode));
	g_assert_cmpint(g_stat(target, &st), ==, 0);
	g_assert_cmpint(st.st_mode & 0777, ==, 0600);

	stdin_log = rig_log(&r, "secret.stdin");
	g_assert_cmpstr(stdin_log, ==, "pw 1");

	gowl_bar_plugin_poll(r.h.plugin);
	g_assert_true(bar_harness_toasted(&r.h, "Saved Office NAS"));
	p3 = bar_harness_panel(&r.h);
	g_assert_true(bar_harness_panel_mentions(p3, "toggle:office-nas"));
	g_assert_null(gowl_bar_panel_find(p3, "add-name"));
	rig_down(&r);
}

/* Catches: a missing tool that crashes, spins or fails silently
   instead of naming the Fedora package that fixes it. */
static void
test_missing_tools(void)
{
	Rig r;
	g_autoptr(GowlBarPanel) p = NULL;
	g_autofree gchar *empty = NULL;

	if (!rig_up(&r))
		return;
	empty = bar_harness_path(&r.h, "empty");
	g_mkdir_with_parents(empty, 0700);
	gowl_bar_plugin_set_setting(r.h.plugin, "helper-path", empty);
	gowl_bar_plugin_set_setting(r.h.plugin, "pkexec", "/nonexistent/pkexec");
	gowl_bar_plugin_set_setting(r.h.plugin, "sshfs", "/nonexistent/sshfs");
	gowl_bar_plugin_set_setting(r.h.plugin, "fusermount",
	                            "/nonexistent/fusermount3");
	gowl_bar_plugin_set_setting(r.h.plugin, "secret-tool",
	                            "/nonexistent/secret-tool");
	rig_config(&r,
		"connections:\n"
		"  - {id: a, name: A, type: cifs, server: s, share: x,"
		" user: u, mountpoint: ~/mnt/a}\n"
		"  - {id: b, name: B, type: nfs, server: s, export: /x,"
		" mountpoint: ~/mnt/b}\n"
		"  - {id: c, name: C, type: sshfs, server: s,"
		" mountpoint: ~/mnt/c}\n");
	bar_harness_poll(&r.h);

	p = bar_harness_panel(&r.h);
	g_assert_true(bar_harness_panel_mentions(p, "cifs-utils"));
	g_assert_true(bar_harness_panel_mentions(p, "nfs-utils"));
	g_assert_true(bar_harness_panel_mentions(p, "fuse-sshfs"));
	g_assert_true(bar_harness_panel_mentions(p, "fuse3"));
	g_assert_true(bar_harness_panel_mentions(p, "polkit"));
	g_assert_true(bar_harness_panel_mentions(p, "libsecret"));

	gowl_bar_plugin_panel_action(r.h.plugin, "toggle:c", -1, 0, 1);
	gowl_bar_plugin_poll(r.h.plugin);
	g_assert_true(bar_harness_toasted(&r.h,
		"Not installed: sshfs (fuse-sshfs)"));
	rig_down(&r);
}

/* Catches: the bar colour not tracking state (green when all expected
   shares are up, yellow when an automount share is down, the idle
   colour when nothing is configured), and the per-widget colour
   settings being ignored or accepting only role names. */
static void
test_colours(void)
{
	Rig r;
	gdouble rgba[4];
	g_autoptr(GowlBarPanel) p = NULL;
	g_autofree gchar *tip = NULL;

	if (!rig_up(&r))
		return;
	bar_harness_poll(&r.h);
	g_assert_cmpint(gowl_bar_plugin_get_color(r.h.plugin), ==,
	                GOWL_BAR_COLOR_SUBTEXT);
	tip = gowl_bar_plugin_dup_tooltip(r.h.plugin);
	g_assert_nonnull(strstr(tip, "none configured"));
	p = bar_harness_panel(&r.h);
	g_assert_true(bar_harness_panel_mentions(p, "Nothing configured"));
	g_assert_true(bar_harness_panel_mentions(p, "add-open"));
	bar_harness_set(&r.h, "color-idle", "#102030");
	g_assert_true(gowl_bar_plugin_get_color_rgba(r.h.plugin, rgba));

	/* An automount share, and its mount fails: yellow. */
	rig_mode(&r, "fail");
	rig_config(&r,
		"connections:\n"
		"  - {id: n, name: N, type: nfs, server: s, export: /x,"
		" mountpoint: ~/mnt/n, automount: true}\n");
	bar_harness_poll(&r.h);
	gowl_bar_plugin_poll(r.h.plugin);
	g_assert_cmpint(gowl_bar_plugin_get_color(r.h.plugin), ==,
	                GOWL_BAR_COLOR_YELLOW);
	g_assert_true(bar_harness_toasted(&r.h, "Could not mount N"));
	bar_harness_set(&r.h, "color-warn", "peach");
	g_assert_cmpint(gowl_bar_plugin_get_color(r.h.plugin), ==,
	                GOWL_BAR_COLOR_PEACH);

	/* It comes up: green, and the override follows. */
	rig_mounted_line(&r, "nfs4", "n");
	gowl_bar_plugin_panel_action(r.h.plugin, "tools", 0, 0, 1);
	gowl_bar_plugin_poll(r.h.plugin);
	g_assert_cmpint(gowl_bar_plugin_get_color(r.h.plugin), ==,
	                GOWL_BAR_COLOR_GREEN);
	bar_harness_set(&r.h, "color-ok", "teal");
	g_assert_cmpint(gowl_bar_plugin_get_color(r.h.plugin), ==,
	                GOWL_BAR_COLOR_TEAL);

	/* show-count off hides the number but not the state. */
	bar_harness_set(&r.h, "show-count", "false");
	{
		g_autofree gchar *label = gowl_bar_plugin_dup_label(r.h.plugin);

		g_assert_null(label);
	}
	rig_down(&r);
}

/* Catches: a malformed file that toasts on every poll, and a broken
   file that leaves the previous connections on screen as if nothing
   had happened. */
static void
test_malformed_file_in_widget(void)
{
	Rig r;
	g_autofree gchar *bad = NULL;
	g_autoptr(GowlBarPanel) p = NULL;
	guint toasts;

	if (!rig_up(&r))
		return;
	bad = fixture_text("malformed.yaml");
	rig_config(&r, bad);
	bar_harness_poll(&r.h);
	bar_harness_poll(&r.h);
	bar_harness_poll(&r.h);
	toasts = r.h.host->toasts->len;
	g_assert_cmpuint(toasts, ==, 1);
	g_assert_true(bar_harness_toasted(&r.h, "has an error"));
	g_assert_cmpint(gowl_bar_plugin_get_color(r.h.plugin), ==,
	                GOWL_BAR_COLOR_YELLOW);
	p = bar_harness_panel(&r.h);
	g_assert_cmpuint(gowl_bar_panel_n_items(p), >, 2);
	rig_down(&r);
}

/* Catches: a job that blocks the action that started it (the action
   runs on the compositor thread), and a row that does not show it is
   busy while the job runs. */
static void
test_busy_while_in_flight(void)
{
	Rig r;
	g_autoptr(GowlBarPanel) p = NULL;
	GowlBarPanelItem *row;
	gint64 t0;

	if (!rig_up(&r))
		return;
	rig_config(&r,
		"connections:\n"
		"  - {id: n, name: N, type: nfs, server: s, export: /x,"
		" mountpoint: ~/mnt/n}\n");
	bar_harness_poll(&r.h);

	r.h.host->hold_work = TRUE;
	t0 = g_get_monotonic_time();
	gowl_bar_plugin_panel_action(r.h.plugin, "toggle:n", -1, 0, 1);
	g_assert_cmpint(g_get_monotonic_time() - t0, <, G_USEC_PER_SEC / 2);
	p = bar_harness_panel(&r.h);
	row = gowl_bar_panel_find(p, "toggle:n");
	g_assert_true(gowl_bar_panel_item_get_busy(row));
	g_assert_cmpint(gowl_bar_plugin_get_interval(r.h.plugin), ==, 1);

	/* A second click while busy queues nothing more. */
	gowl_bar_plugin_panel_action(r.h.plugin, "toggle:n", -1, 0, 1);
	g_assert_cmpuint(r.h.host->held->len, ==, 1);
	g_assert_cmpuint(bar_harness_run_held(&r.h), ==, 1);
	r.h.host->hold_work = FALSE;
	gowl_bar_plugin_poll(r.h.plugin);
	g_assert_true(bar_harness_toasted(&r.h, "Mounted N"));
	rig_down(&r);
}

/* Catches the shape of a future regression no runtime test sees: a
   password variable added to an argv array.  Every argv in the
   widget is built with g_ptr_array_add(argv, ...), so no such line
   may mention a password or a secret. */
static void
test_source_guard(void)
{
	static const gchar *const files[] = {
		"bar-plugins-netshares.c", "bar-netshares-model.c", NULL
	};
	guint f;

	for (f = 0; files[f] != NULL; f++) {
		g_autofree gchar *path = NULL;
		g_autofree gchar *text = NULL;
		g_auto(GStrv) lines = NULL;
		guint i, argv_lines = 0;

		path = g_build_filename(GOWL_TEST_FIXTURES, "..", "..", "..",
		                        "modules", "bar", files[f], NULL);
		if (!g_file_get_contents(path, &text, NULL, NULL)) {
			g_test_skip("source not found");
			return;
		}
		lines = g_strsplit(text, "\n", -1);
		for (i = 0; lines[i] != NULL; i++) {
			g_autofree gchar *low = NULL;

			if (strstr(lines[i], "g_ptr_array_add(argv") == NULL)
				continue;
			argv_lines++;
			low = g_ascii_strdown(lines[i], -1);
			if (strstr(low, "password") != NULL ||
			    strstr(low, "secret") != NULL)
				g_error("%s:%u puts a secret in argv: %s",
				        files[f], i + 1, lines[i]);
		}
		if (f == 1)
			g_assert_cmpuint(argv_lines, >, 10);
	}
}

int
main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/bar-netshares/load/valid", test_load_valid);
	g_test_add_func("/bar-netshares/load/empty-and-malformed",
	                test_load_empty_and_malformed);
	g_test_add_func("/bar-netshares/load/hostile", test_load_hostile);
	g_test_add_func("/bar-netshares/option-filter", test_option_filter);
	g_test_add_func("/bar-netshares/mountpoint", test_mountpoint_validation);
	g_test_add_func("/bar-netshares/mountinfo", test_mountinfo);
	g_test_add_func("/bar-netshares/argv/cifs", test_argv_cifs);
	g_test_add_func("/bar-netshares/argv/nfs", test_argv_nfs);
	g_test_add_func("/bar-netshares/argv/sshfs", test_argv_sshfs);
	g_test_add_func("/bar-netshares/argv/unmount-and-secret",
	                test_argv_unmount_and_secret);
	g_test_add_func("/bar-netshares/credentials", test_credentials);
	g_test_add_func("/bar-netshares/run/timeout", test_run_timeout);
	g_test_add_func("/bar-netshares/run/stdin-hazards",
	                test_run_stdin_hazards);
	g_test_add_func("/bar-netshares/last-line", test_last_line);
	g_test_add_func("/bar-netshares/widget/cifs-password-out-of-sight",
	                test_mount_cifs_keeps_the_password_out_of_sight);
	g_test_add_func("/bar-netshares/widget/failures-say-why",
	                test_mount_failures_say_why);
	g_test_add_func("/bar-netshares/widget/timeout", test_mount_timeout);
	g_test_add_func("/bar-netshares/widget/password-on-stdin",
	                test_store_password_on_stdin);
	g_test_add_func("/bar-netshares/widget/unmount-busy-lazy",
	                test_unmount_busy_offers_lazy);
	g_test_add_func("/bar-netshares/widget/sshfs", test_sshfs_round_trip);
	g_test_add_func("/bar-netshares/widget/forget", test_forget);
	g_test_add_func("/bar-netshares/widget/add-form", test_add_form);
	g_test_add_func("/bar-netshares/widget/missing-tools",
	                test_missing_tools);
	g_test_add_func("/bar-netshares/widget/colours", test_colours);
	g_test_add_func("/bar-netshares/widget/malformed-file",
	                test_malformed_file_in_widget);
	g_test_add_func("/bar-netshares/widget/busy-in-flight",
	                test_busy_while_in_flight);
	g_test_add_func("/bar-netshares/source-guard", test_source_guard);
	return g_test_run();
}
