/* test-fx-optout.c -- the windows that get no effects
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Three questions with one answer, and each fails differently.
 *
 *   THE VARIABLE has to be read out of the PROCESS, not out of the
 *   compositor.  That is what makes `GOWL_NO_FX=1' inherited -- export
 *   it in a shell and everything started from that shell is covered --
 *   and it is invisible to a test that reads its own g_getenv().  The
 *   cases below spawn real children and ask about THEM.
 *
 *   THE VALUE has to be exactly the yes-es.  A variable this coarse is
 *   set and unset in wrapper scripts, and `GOWL_NO_FX=0' meaning "no
 *   effects" would make it impossible to turn back on for one child of
 *   a process that has it on.
 *
 *   THE ANCESTRY is the whole of the Steam story: a game is three or
 *   four processes below the client, and the interesting failure is
 *   not "we missed one" but the OPPOSITE -- a walk that runs past the
 *   compositor and matches something above it would switch the effects
 *   off for every window on the desktop.  There is a case for each
 *   direction.
 *
 * Nothing here needs a compositor, a scene graph or a GPU: the decision
 * is a pure function of /proc and two strings, which is why it lives in
 * a translation unit of its own.
 */

#include <glib.h>
#include <glib/gstdio.h>
#include <signal.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "util/gowl-fx-optout.h"

/*
 * An environment block the way /proc gives it: entries separated by
 * NUL, and NOT necessarily terminated by one.
 *
 * Built by hand rather than with g_strjoinv() because the separator is
 * the thing being tested.  @tail_nul is what tells the two shapes
 * apart: a block that ends without one is what a process rewriting its
 * own environ leaves behind, and reading it with strlen() walks off the
 * end of the buffer.
 */
static gchar *
environ_blob(const gchar *const *entries, gboolean tail_nul, gsize *out_len)
{
	GString *s = g_string_new(NULL);
	guint    i;

	for (i = 0; entries[i] != NULL; i++) {
		g_string_append(s, entries[i]);
		if (entries[i + 1] != NULL || tail_nul)
			g_string_append_c(s, '\0');
	}
	*out_len = s->len;
	return g_string_free(s, FALSE);
}

static gboolean
env_says_no(const gchar *const *entries)
{
	g_autofree gchar *blob = NULL;
	gsize len = 0;

	blob = environ_blob(entries, TRUE, &len);
	return gowl_fx_optout_environ_no_fx(blob, len);
}

/* ── The variable ────────────────────────────────────────────────── */

/*
 * `1' and `true' in any case, and nothing else.
 *
 * The `0'/`false' half is not pedantry: a wrapper script that turns the
 * effects back on for one program does it by setting the variable to a
 * no, because it cannot unset what its parent exported for it.
 */
static void
test_only_a_yes_means_yes(void)
{
	const gchar *const yes_1[]    = { "PATH=/usr/bin", "GOWL_NO_FX=1", NULL };
	const gchar *const yes_true[] = { "GOWL_NO_FX=true", NULL };
	const gchar *const yes_TRUE[] = { "GOWL_NO_FX=TRUE", NULL };
	const gchar *const yes_mixed[]= { "GOWL_NO_FX=True", NULL };
	const gchar *const no_zero[]  = { "GOWL_NO_FX=0", NULL };
	const gchar *const no_false[] = { "GOWL_NO_FX=false", NULL };
	const gchar *const no_empty[] = { "GOWL_NO_FX=", NULL };
	const gchar *const no_absent[]= { "HOME=/root", "TERM=gst", NULL };

	g_assert_true(env_says_no(yes_1));
	g_assert_true(env_says_no(yes_true));
	g_assert_true(env_says_no(yes_TRUE));
	g_assert_true(env_says_no(yes_mixed));

	g_assert_false(env_says_no(no_zero));
	g_assert_false(env_says_no(no_false));
	g_assert_false(env_says_no(no_empty));
	g_assert_false(env_says_no(no_absent));
}

/*
 * The NAME has to match exactly, not as a prefix.
 *
 * `GOWL_NO_FX_DEBUG=1' is the sort of thing that appears next to a
 * variable like this, and a prefix match would read it as the variable
 * itself.
 */
static void
test_a_longer_name_is_a_different_variable(void)
{
	const gchar *const longer[] = { "GOWL_NO_FX_DEBUG=1", NULL };
	const gchar *const shorter[] = { "GOWL_NO_F=1", NULL };
	const gchar *const suffix[] = { "MY_GOWL_NO_FX=1", NULL };
	const gchar *const steamish[] = { "SteamAppIdList=440", NULL };
	g_autofree gchar *blob = NULL;
	gsize len = 0;

	g_assert_false(env_says_no(longer));
	g_assert_false(env_says_no(shorter));
	g_assert_false(env_says_no(suffix));

	/* The same boundary on the Steam side, where it bites harder: those
	 * variables are tested for being NON-EMPTY rather than for a
	 * particular value, so a prefix match would read the rest of a
	 * longer name as the value and answer yes. */
	blob = environ_blob(steamish, TRUE, &len);
	g_assert_false(gowl_fx_optout_environ_steam(blob, len));
}

/*
 * A block that does not end in a NUL is read to its end and no further.
 *
 * /proc/PID/environ is not guaranteed to be terminated, and the natural
 * way to write this loop -- strlen() each entry -- reads past the
 * buffer on the last one.  Under the address sanitiser that is a
 * crash; without it, it is whatever happened to be next in the heap.
 */
static void
test_an_unterminated_block_is_still_read(void)
{
	g_autofree gchar *blob = NULL;
	const gchar *const entries[] = { "A=b", "GOWL_NO_FX=1", NULL };
	gsize len = 0;

	blob = environ_blob(entries, FALSE, &len);
	g_assert_cmpuint(len, ==, strlen("A=b") + 1 + strlen("GOWL_NO_FX=1"));
	g_assert_true(gowl_fx_optout_environ_no_fx(blob, len));

	/* And the degenerate inputs answer rather than crash. */
	g_assert_false(gowl_fx_optout_environ_no_fx(NULL, 0));
	g_assert_false(gowl_fx_optout_environ_no_fx("", 0));
}

/* ── Steam ───────────────────────────────────────────────────────── */

/*
 * Steam is recognised by what it exports, one variable at a time.
 *
 * Each of the four is a different situation -- a native game, a game
 * launched from the library, something started from the client that is
 * not a game, and a Proton prefix -- so each is asserted on its own
 * rather than all together, where three could be broken and the case
 * still pass.
 */
static void
test_steam_is_recognised_by_what_it_exports(void)
{
	const gchar *const vars[] = {
		"SteamAppId=440",
		"SteamGameId=440",
		"SteamClientLaunch=1",
		"STEAM_COMPAT_DATA_PATH=/home/z/.steam/steamapps/compatdata/440",
		NULL
	};
	guint i;

	for (i = 0; vars[i] != NULL; i++) {
		const gchar *one[2];
		g_autofree gchar *blob = NULL;
		gsize len = 0;

		one[0] = vars[i];
		one[1] = NULL;
		blob = environ_blob((const gchar *const *)one, TRUE, &len);
		g_assert_true(gowl_fx_optout_environ_steam(blob, len));
	}
}

/*
 * An empty value is not a Steam environment.
 *
 * `SteamAppId=' with nothing after it is what a shell leaves when it
 * exports a variable it never set, and treating it as a yes would
 * switch the effects off for every child of that shell.
 */
static void
test_an_empty_steam_variable_is_not_steam(void)
{
	const gchar *const empty[] = { "SteamAppId=", NULL };
	g_autofree gchar *blob = NULL;
	gsize len = 0;

	blob = environ_blob(empty, TRUE, &len);
	g_assert_false(gowl_fx_optout_environ_steam(blob, len));
}

/* ── The name list ───────────────────────────────────────────────── */

/*
 * A hand-written list behaves: spacing, case and a trailing comma.
 *
 * All three are what a config file actually contains, and each of them
 * silently matching nothing is the failure -- the user sees the
 * bubbles still there and no error anywhere.
 */
static void
test_the_list_forgives_how_it_was_typed(void)
{
	const gchar *list = " obs , VLC,mpv, ";

	g_assert_true(gowl_fx_optout_name_listed("obs", list));
	g_assert_true(gowl_fx_optout_name_listed("vlc", list));
	g_assert_true(gowl_fx_optout_name_listed("MPV", list));
	g_assert_false(gowl_fx_optout_name_listed("firefox", list));

	/* An empty entry must not match an empty name, or a list ending in
	 * a comma would match a window with no app_id -- which is most
	 * XWayland windows before they set their class. */
	g_assert_false(gowl_fx_optout_name_listed("", list));
	g_assert_false(gowl_fx_optout_name_listed(NULL, list));
	g_assert_false(gowl_fx_optout_name_listed("obs", NULL));
	g_assert_false(gowl_fx_optout_name_listed("obs", ""));
}

/*
 * Steam and mutter-devkit are BUILT IN, not defaults.
 *
 * The difference matters: a default is something a config replaces, and
 * a user who writes their own `no-fx-apps' would then quietly put the
 * bubbles back on their games.  The list function itself does not know
 * these names -- which is the other half of the assertion, and the one
 * that says where the knowledge lives.
 */
static void
test_the_built_in_names_are_not_in_the_list(void)
{
	g_assert_false(gowl_fx_optout_name_listed("steam", NULL));
	g_assert_false(gowl_fx_optout_name_listed("mutter-devkit", NULL));

	/* But the decision knows them, with no configuration and no pid. */
	g_assert_true(gowl_fx_optout_for_pid(-1, "steam", NULL));
	g_assert_true(gowl_fx_optout_for_pid(-1, "Steam", NULL));
	g_assert_true(gowl_fx_optout_for_pid(-1, "steamwebhelper", NULL));
	g_assert_true(gowl_fx_optout_for_pid(-1, "mutter-devkit", NULL));

	/* And it still knows them when a config replaced the list with
	 * something else entirely. */
	g_assert_true(gowl_fx_optout_for_pid(-1, "steam", "obs,vlc"));
	g_assert_true(gowl_fx_optout_for_pid(-1, "obs", "obs,vlc"));
	g_assert_false(gowl_fx_optout_for_pid(-1, "firefox", "obs,vlc"));
}

/* ── Real processes ──────────────────────────────────────────────── */

/*
 * A child, with an environment of our choosing.
 *
 * G_SPAWN_DO_NOT_REAP_CHILD so the pid stays valid for as long as the
 * case needs /proc to answer about it; `sleep' so it is alive without
 * doing anything, and briefly, so a case that dies before its cleanup
 * leaves nothing behind for more than a few seconds.
 */
static GPid
spawn_sleeper(gchar **envp, gint *out_stdout)
{
	gchar  *argv[] = { (gchar *)"/bin/sh", (gchar *)"-c", NULL, NULL };
	GPid    pid = 0;
	GError *error = NULL;

	/* The shell prints the pid of its own child and then waits on it,
	 * which is the only way to get at a GRANDCHILD -- and a grandchild
	 * is what the ancestry cases need, since the walk stops at this
	 * process. */
	argv[2] = (gchar *)"sleep 20 & echo $!; wait";

	if (!g_spawn_async_with_pipes(NULL, argv, envp,
	                              G_SPAWN_DO_NOT_REAP_CHILD,
	                              NULL, NULL, &pid,
	                              NULL, out_stdout, NULL, &error)) {
		g_test_skip(error->message);
		g_clear_error(&error);
		return 0;
	}
	return pid;
}

/* The pid the shell printed, i.e. the grandchild. */
static pid_t
read_grandchild(gint fd)
{
	gchar buf[64];
	gssize n;

	n = read(fd, buf, sizeof(buf) - 1);
	if (n <= 0)
		return 0;
	buf[n] = '\0';
	return (pid_t)g_ascii_strtoll(buf, NULL, 10);
}

/*
 * Wait until @pid's command name is @want.
 *
 * The shell prints `$!' the moment it has FORKED, which is before the
 * child has EXEC'd -- so for a few microseconds the grandchild's name
 * is still the shell's.  Reading it in that window is a race the
 * ancestry case lost the first time it was written: the grandchild
 * looked like another `sh' and the case appeared to say the walk had
 * matched the parent when it had matched the child.
 *
 * Returns %FALSE if it never happens, which is a skip rather than a
 * failure: it means the machine did something unusual, not that the
 * walk is wrong.
 */
static gboolean
wait_for_comm(pid_t pid, const gchar *want)
{
	guint tries;

	for (tries = 0; tries < 200; tries++) {
		g_autofree gchar *path = g_strdup_printf("/proc/%d/comm", (gint)pid);
		g_autofree gchar *comm = NULL;

		if (g_file_get_contents(path, &comm, NULL, NULL)) {
			g_strstrip(comm);
			if (g_strcmp0(comm, want) == 0)
				return TRUE;
		}
		g_usleep(1000);
	}
	return FALSE;
}

static void
reap(GPid pid, pid_t grandchild, gint fd)
{
	/* The grandchild first: killing the shell alone would orphan it,
	 * and it would then outlive the test run by its whole sleep. */
	if (grandchild > 0)
		kill(grandchild, SIGTERM);
	if (pid > 0) {
		kill(pid, SIGTERM);
		waitpid(pid, NULL, 0);
		g_spawn_close_pid(pid);
	}
	if (fd >= 0)
		close(fd);
}

/*
 * The variable is INHERITED, which is the entire point of reading it
 * out of /proc.
 *
 * A child that has it gets no effects and a child that does not keeps
 * them -- the second half being the one that catches a decision that
 * accidentally answers yes for everything.
 */
static void
test_the_variable_travels_to_children(void)
{
	gchar *with[] = { (gchar *)"GOWL_NO_FX=1", (gchar *)"PATH=/usr/bin:/bin", NULL };
	gchar *without[] = { (gchar *)"PATH=/usr/bin:/bin", NULL };
	GPid   a, b;
	gint   fd_a = -1, fd_b = -1;
	pid_t  ga, gb;

	a = spawn_sleeper(with, &fd_a);
	if (a == 0)
		return;
	b = spawn_sleeper(without, &fd_b);
	if (b == 0) {
		reap(a, 0, fd_a);
		return;
	}
	ga = read_grandchild(fd_a);
	gb = read_grandchild(fd_b);

	g_assert_true(gowl_fx_optout_for_pid((pid_t)a, NULL, NULL));
	g_assert_false(gowl_fx_optout_for_pid((pid_t)b, NULL, NULL));

	/* And it reached the grandchild too, which is what "inherited"
	 * means once there is a launcher in between. */
	if (ga > 0)
		g_assert_true(gowl_fx_optout_for_pid(ga, NULL, NULL));
	if (gb > 0)
		g_assert_false(gowl_fx_optout_for_pid(gb, NULL, NULL));

	reap(a, ga, fd_a);
	reap(b, gb, fd_b);
}

/*
 * A Steam environment needs no configuration and no name.
 *
 * This is the case that covers an actual game: the window belongs to a
 * process whose app_id we have never heard of, and the only thing
 * saying "this is a game" is what Steam put in its environment.
 */
static void
test_a_steam_environment_covers_a_game_we_cannot_name(void)
{
	gchar *env[] = {
		(gchar *)"SteamAppId=440",
		(gchar *)"PATH=/usr/bin:/bin",
		NULL
	};
	GPid  pid;
	gint  fd = -1;
	pid_t grandchild;

	pid = spawn_sleeper(env, &fd);
	if (pid == 0)
		return;
	grandchild = read_grandchild(fd);

	g_assert_true(gowl_fx_optout_for_pid((pid_t)pid, "hl2_linux", NULL));
	if (grandchild > 0)
		g_assert_true(gowl_fx_optout_for_pid(grandchild, "hl2_linux", NULL));

	reap(pid, grandchild, fd);
}

/*
 * A named LAUNCHER covers what it launches.
 *
 * The grandchild is a `sleep' -- nothing about it matches anything --
 * and it is found by walking up to the shell that started it.  This is
 * the mechanism that catches the Steam client's own windows and a
 * nested GNOME's, where the process holding the surface is not the
 * process anybody would think to name.
 */
static void
test_a_launcher_covers_what_it_launched(void)
{
	gchar *env[] = { (gchar *)"PATH=/usr/bin:/bin", NULL };
	GPid   pid;
	gint   fd = -1;
	pid_t  grandchild;

	pid = spawn_sleeper(env, &fd);
	if (pid == 0)
		return;
	grandchild = read_grandchild(fd);
	if (grandchild <= 0) {
		reap(pid, 0, fd);
		g_test_skip("could not learn the grandchild's pid");
		return;
	}

	if (!wait_for_comm(grandchild, "sleep")) {
		reap(pid, grandchild, fd);
		g_test_skip("the grandchild never became `sleep'");
		return;
	}

	/* The grandchild is a `sleep' and its parent is the shell.  Naming
	 * the SHELL is enough, which is the whole mechanism: the process
	 * holding the surface is not the process anybody would name. */
	g_assert_true(gowl_fx_optout_for_pid(grandchild, NULL, "sh"));

	/* Naming the process itself also works -- the walk starts at it,
	 * not at its parent. */
	g_assert_true(gowl_fx_optout_for_pid(grandchild, NULL, "sleep"));

	/* And a list that names neither leaves it alone.  Without this the
	 * case above would pass for a walk that answered yes to
	 * everything. */
	g_assert_false(gowl_fx_optout_for_pid(grandchild, NULL,
	                                      "no-such-process-anywhere"));

	reap(pid, grandchild, fd);
}

/*
 * The walk stops at the compositor.
 *
 * This is the dangerous direction.  gowl is embedded in cmacs and cmacs
 * is started from a shell, so a walk that ran all the way to pid 1
 * would find `sh', `systemd' and whatever else is above the compositor
 * for EVERY window -- and naming any of them in `no-fx-apps' would
 * switch the effects off across the whole desktop instead of for one
 * program.
 *
 * The test process stands in for the compositor: a child of it must not
 * match this process's own name.
 */
static void
test_the_walk_stops_at_the_compositor(void)
{
	gchar *env[] = { (gchar *)"PATH=/usr/bin:/bin", NULL };
	g_autofree gchar *own_comm = NULL;
	GPid   pid;
	gint   fd = -1;
	pid_t  grandchild;

	if (!g_file_get_contents("/proc/self/comm", &own_comm, NULL, NULL)) {
		g_test_skip("no /proc");
		return;
	}
	g_strstrip(own_comm);

	pid = spawn_sleeper(env, &fd);
	if (pid == 0)
		return;
	grandchild = read_grandchild(fd);

	/* Our own name is two hops above the grandchild and one above the
	 * child, and neither of them may match it. */
	g_assert_false(gowl_fx_optout_for_pid((pid_t)pid, NULL, own_comm));
	if (grandchild > 0)
		g_assert_false(gowl_fx_optout_for_pid(grandchild, NULL, own_comm));

	reap(pid, grandchild, fd);
}

/*
 * A pid that is not there answers no.
 *
 * Every /proc read here can fail -- the process exits between the map
 * and the read, a container hides it, a hardened kernel refuses it --
 * and the answer has to be "leave the window alone", which is what the
 * desktop did before any of this existed.
 */
static void
test_an_unreadable_process_keeps_its_effects(void)
{
	g_assert_false(gowl_fx_optout_for_pid(-1, NULL, NULL));
	g_assert_false(gowl_fx_optout_for_pid(0, "firefox", "obs"));
	/* A pid far past any plausible live one. */
	g_assert_false(gowl_fx_optout_for_pid(0x7ffffff0, "firefox", "obs"));
}

gint
main(gint argc, gchar **argv)
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/fx-optout/only-a-yes-means-yes",
	                test_only_a_yes_means_yes);
	g_test_add_func("/fx-optout/a-longer-name-is-a-different-variable",
	                test_a_longer_name_is_a_different_variable);
	g_test_add_func("/fx-optout/an-unterminated-block-is-still-read",
	                test_an_unterminated_block_is_still_read);
	g_test_add_func("/fx-optout/steam-is-recognised-by-what-it-exports",
	                test_steam_is_recognised_by_what_it_exports);
	g_test_add_func("/fx-optout/an-empty-steam-variable-is-not-steam",
	                test_an_empty_steam_variable_is_not_steam);
	g_test_add_func("/fx-optout/the-list-forgives-how-it-was-typed",
	                test_the_list_forgives_how_it_was_typed);
	g_test_add_func("/fx-optout/the-built-in-names-are-not-in-the-list",
	                test_the_built_in_names_are_not_in_the_list);
	g_test_add_func("/fx-optout/the-variable-travels-to-children",
	                test_the_variable_travels_to_children);
	g_test_add_func("/fx-optout/a-steam-environment-covers-a-game",
	                test_a_steam_environment_covers_a_game_we_cannot_name);
	g_test_add_func("/fx-optout/a-launcher-covers-what-it-launched",
	                test_a_launcher_covers_what_it_launched);
	g_test_add_func("/fx-optout/the-walk-stops-at-the-compositor",
	                test_the_walk_stops_at_the_compositor);
	g_test_add_func("/fx-optout/an-unreadable-process-keeps-its-effects",
	                test_an_unreadable_process_keeps_its_effects);

	return g_test_run();
}
