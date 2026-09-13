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

#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "gowl-lock"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <string.h>
#include <unistd.h>

#include <glib.h>

#include "lock.h"

#define LICENSE_TEXT \
"gowl-lock -- the gowl screen lock\n" \
"Copyright (C) 2026  Zach Podbielniak\n" \
"\n" \
"This program is free software: you can redistribute it and/or modify\n" \
"it under the terms of the GNU Affero General Public License as\n" \
"published by the Free Software Foundation, either version 3 of the\n" \
"License, or (at your option) any later version.\n" \
"\n" \
"This program is distributed in the hope that it will be useful, but\n" \
"WITHOUT ANY WARRANTY; without even the implied warranty of\n" \
"MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU\n" \
"Affero General Public License for more details.\n" \
"\n" \
"You should have received a copy of the GNU Affero General Public\n" \
"License along with this program.  If not, see\n" \
"<https://www.gnu.org/licenses/>.\n"

/* ------------------------------------------------------------------
 * Colours
 * ------------------------------------------------------------------ */

/* "#rrggbb" or "#rrggbbaa" into four doubles.  A string that does not
 * parse leaves the default alone and says so, rather than turning the
 * lock screen an unexpected colour or, worse, transparent. */
static void
parse_color(const gchar *spec, gdouble out[4], const gchar *what)
{
	guint r, g, b, a = 0xFF;
	gsize len;

	if (spec == NULL)
		return;
	if (spec[0] == '#')
		spec++;
	len = strlen(spec);
	if (len == 6 && sscanf(spec, "%2x%2x%2x", &r, &g, &b) == 3) {
		/* no alpha given: opaque */
	} else if (len == 8
	           && sscanf(spec, "%2x%2x%2x%2x", &r, &g, &b, &a) == 4) {
		/* alpha given */
	} else {
		g_warning("%s: '%s' is not #rrggbb or #rrggbbaa; keeping the "
		          "default", what, spec);
		return;
	}
	out[0] = r / 255.0;
	out[1] = g / 255.0;
	out[2] = b / 255.0;
	out[3] = a / 255.0;
}

/* ------------------------------------------------------------------
 * PAM service
 * ------------------------------------------------------------------ */

/*
 * The first PAM service that exists.
 *
 * Refusing to start when there is none is deliberate: a lock screen with
 * no way to authenticate is a screen nobody can get past, and finding
 * that out at the lock screen is the worst possible moment.
 */
static const gchar *
resolve_pam_service(void)
{
	static const gchar *const candidates[] = {
		"gowl", "cmacs", "swaylock", "system-auth", "login", NULL
	};
	gint i;

	for (i = 0; candidates[i] != NULL; i++) {
		g_autofree gchar *path =
			g_strconcat("/etc/pam.d/", candidates[i], NULL);

		if (g_file_test(path, G_FILE_TEST_EXISTS))
			return candidates[i];
	}
	return NULL;
}

/* ------------------------------------------------------------------
 * The loop
 * ------------------------------------------------------------------ */

/*
 * Two file descriptors: the Wayland connection, and the pipe PAM's
 * thread answers on.
 *
 * The Wayland side is the prepare_read dance rather than a plain
 * dispatch, because this process has a second thread and the simple
 * version races it: wl_display_dispatch() reads and dispatches as one
 * step, and anything queued between the poll and the read would be
 * dispatched out from under a reader that had not announced itself.
 */
static int
run(GowlLock *self)
{
	while (self->running) {
		struct pollfd fds[2];
		gint n = 0;
		gint wl_fd;

		while (wl_display_prepare_read(self->display) != 0) {
			if (wl_display_dispatch_pending(self->display) < 0)
				return 1;
		}
		if (wl_display_flush(self->display) < 0 && errno != EAGAIN) {
			wl_display_cancel_read(self->display);
			g_warning("the compositor connection broke");
			return 1;
		}

		wl_fd = wl_display_get_fd(self->display);
		fds[n].fd = wl_fd;
		fds[n].events = POLLIN;
		fds[n].revents = 0;
		n++;
		fds[n].fd = self->auth_pipe[0];
		fds[n].events = POLLIN;
		fds[n].revents = 0;
		n++;

		if (poll(fds, (nfds_t)n, -1) < 0) {
			wl_display_cancel_read(self->display);
			if (errno == EINTR)
				continue;
			return 1;
		}

		if ((fds[0].revents & POLLIN) != 0)
			wl_display_read_events(self->display);
		else
			wl_display_cancel_read(self->display);

		if (wl_display_dispatch_pending(self->display) < 0) {
			g_warning("the compositor connection broke");
			return 1;
		}

		if ((fds[1].revents & POLLIN) != 0) {
			gchar answer = 'n';
			ssize_t r = read(self->auth_pipe[0], &answer, 1);

			if (r == 1)
				gowl_lock_auth_result(self, answer == 'y');
		}
	}
	return self->finished ? 1 : 0;
}

/* ------------------------------------------------------------------
 * main
 * ------------------------------------------------------------------ */

int
main(int argc, char **argv)
{
	GowlLock lock;
	g_autoptr(GOptionContext) ctx = NULL;
	g_autoptr(GError) error = NULL;
	gboolean show_license = FALSE;
	gchar *image = NULL;
	gchar *mode = NULL;
	gchar *service = NULL;
	gchar *font = NULL;
	gchar *bg = NULL;
	gchar *fg = NULL;
	gchar *accent = NULL;
	gchar *err_color = NULL;
	gdouble font_size = 0.0;
	gboolean show_failures = FALSE;
	gint ready_fd = -1;
	gint rc;

	const GOptionEntry entries[] = {
		{ "image", 'i', 0, G_OPTION_ARG_FILENAME, &image,
		  "Picture to show behind the prompt", "PATH" },
		{ "mode", 'm', 0, G_OPTION_ARG_STRING, &mode,
		  "How to scale it: fill, fit, center, stretch (default fill)",
		  "MODE" },
		{ "pam-service", 's', 0, G_OPTION_ARG_STRING, &service,
		  "PAM service to authenticate against (default: the first of "
		  "gowl, cmacs, swaylock, system-auth, login that exists)",
		  "NAME" },
		{ "color", 'c', 0, G_OPTION_ARG_STRING, &bg,
		  "Background colour, #rrggbb[aa] (default #1e1e2e)", "COLOR" },
		{ "text-color", 0, 0, G_OPTION_ARG_STRING, &fg,
		  "Text colour (default #cdd6f4)", "COLOR" },
		{ "indicator-color", 0, 0, G_OPTION_ARG_STRING, &accent,
		  "Ring colour (default #89b4fa)", "COLOR" },
		{ "error-color", 0, 0, G_OPTION_ARG_STRING, &err_color,
		  "Ring colour after a refusal (default #f38ba8)", "COLOR" },
		{ "font", 0, 0, G_OPTION_ARG_STRING, &font,
		  "Font family (default monospace)", "NAME" },
		{ "font-size", 0, 0, G_OPTION_ARG_DOUBLE, &font_size,
		  "Font size in points (default 20)", "SIZE" },
		{ "show-failed-attempts", 'f', 0, G_OPTION_ARG_NONE,
		  &show_failures,
		  "Show how many attempts have been refused", NULL },
		{ "ready-fd", 0, 0, G_OPTION_ARG_INT, &ready_fd,
		  "Write a byte to this fd once the session is really locked, "
		  "then close it", "FD" },
		{ "license", 0, 0, G_OPTION_ARG_NONE, &show_license,
		  "Print the licence and exit", NULL },
		{ NULL, 0, 0, 0, NULL, NULL, NULL }
	};

	ctx = g_option_context_new("- lock the gowl session");
	g_option_context_set_summary(ctx,
		"Locks the screen and asks for your password.\n"
		"\n"
		"Holds the session through ext-session-lock-v1, so the desktop\n"
		"stays hidden even if this program crashes -- gowl keeps the\n"
		"session locked and starts a new lock screen.\n"
		"\n"
		"Examples:\n"
		"  gowl-lock\n"
		"  gowl-lock -i ~/Pictures/lock.png -m fill\n"
		"  gowl-lock -c '#11111b' --indicator-color '#a6e3a1'\n"
		"  gowl-lock -f -s system-auth\n"
		"\n"
		"gowl normally starts this for you: it is the `lock-command'\n"
		"in the config, and Super+Shift+l, `gowl-msg lock', the idle\n"
		"timer and a suspend all go through it.");
	g_option_context_add_main_entries(ctx, entries, NULL);
	if (!g_option_context_parse(ctx, &argc, &argv, &error)) {
		g_printerr("%s\n", error->message);
		return 1;
	}
	if (show_license) {
		g_print("%s", LICENSE_TEXT);
		return 0;
	}

	memset(&lock, 0, sizeof lock);
	lock.auth_pipe[0] = lock.auth_pipe[1] = -1;
	lock.ready_fd  = ready_fd;
	lock.phase     = GOWL_LOCK_IDLE;
	lock.font      = g_strdup(font != NULL ? font : "monospace");
	lock.font_size = font_size > 0.0 ? font_size : 20.0;
	lock.image_path = g_strdup(image);
	lock.image_mode = g_strdup(mode != NULL ? mode : "fill");
	lock.show_failures = show_failures;

	/* Catppuccin Mocha, matching the compositor's own defaults so the
	 * lock screen looks like the desktop it is covering. */
	parse_color("#1e1e2e", lock.bg, "color");
	parse_color("#cdd6f4", lock.fg, "text-color");
	parse_color("#89b4fa", lock.accent, "indicator-color");
	parse_color("#f38ba8", lock.error, "error-color");
	parse_color(bg, lock.bg, "color");
	parse_color(fg, lock.fg, "text-color");
	parse_color(accent, lock.accent, "indicator-color");
	parse_color(err_color, lock.error, "error-color");

	lock.pam_service = g_strdup(service != NULL ? service
	                            : resolve_pam_service());
	if (lock.pam_service == NULL) {
		g_printerr("gowl-lock: no PAM service in /etc/pam.d -- refusing "
		           "to lock a screen nothing could unlock.\n"
		           "Install one (`sudo make install-pam' in gowl, or "
		           "`sudo make install-cmacs-pam' in cmacs), or name an "
		           "existing service with --pam-service.\n");
		return 1;
	}

	if (pipe(lock.auth_pipe) != 0) {
		g_printerr("gowl-lock: pipe: %s\n", g_strerror(errno));
		return 1;
	}
	fcntl(lock.auth_pipe[0], F_SETFD, FD_CLOEXEC);
	fcntl(lock.auth_pipe[1], F_SETFD, FD_CLOEXEC);

	if (!gowl_lock_connect(&lock, &error)) {
		g_printerr("gowl-lock: %s\n", error->message);
		return 1;
	}
	gowl_lock_load_image(&lock);
	gowl_lock_engage(&lock);

	rc = run(&lock);

	/* The password never reaches the exit path, but the buffer that held
	 * it might still be mapped, so it is wiped rather than merely
	 * freed. */
	gowl_lock_auth_join(&lock);
	gowl_lock_password_clear(&lock);
	g_free(lock.password);
	gowl_lock_free_image(&lock);
	gowl_lock_disconnect(&lock);
	if (lock.auth_pipe[0] >= 0)
		close(lock.auth_pipe[0]);
	if (lock.auth_pipe[1] >= 0)
		close(lock.auth_pipe[1]);
	g_free(lock.pam_service);
	g_free(lock.image_path);
	g_free(lock.image_mode);
	g_free(lock.font);
	g_free(image);
	g_free(mode);
	g_free(service);
	g_free(font);
	g_free(bg);
	g_free(fg);
	g_free(accent);
	g_free(err_color);
	return rc;
}
