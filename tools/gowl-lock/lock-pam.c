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
 * Authentication.
 *
 * pam_authenticate() blocks -- for a second on a bad password, for as
 * long as a fingerprint reader waits on a finger -- so it runs on a
 * thread, and the answer comes back down a pipe the main loop is already
 * polling.  A single byte: 'y' or 'n'.
 *
 * This is the code that made the lock worth pulling out of the
 * compositor.  PAM dlopens whatever the system's stack names, into the
 * calling process; under `cmacs --gowl' that process is the editor, and
 * a pam_fprintd that segfaults would take the desktop and every unsaved
 * buffer with it.  Here the worst case is this program dying, which
 * leaves the session locked and gets a new lock screen started.
 */

#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "gowl-lock"

#include <security/pam_appl.h>
#include <string.h>
#include <unistd.h>

#include "lock.h"

/*
 * Hand PAM the password it asks for.
 *
 * The reply array and each string in it are freed by PAM with free(),
 * so they are malloc'd rather than g_malloc'd -- the two happen to be
 * the same allocator here, but relying on that is how a port to a
 * platform where they are not becomes a crash nobody can reproduce.
 */
static int
conversation(int n, const struct pam_message **msgs,
             struct pam_response **resp, void *data)
{
	const gchar *password = (const gchar *)data;
	struct pam_response *replies;
	int i;

	if (n <= 0 || msgs == NULL || resp == NULL)
		return PAM_CONV_ERR;

	replies = (struct pam_response *)calloc((size_t)n,
	                                        sizeof(struct pam_response));
	if (replies == NULL)
		return PAM_BUF_ERR;

	for (i = 0; i < n; i++) {
		switch (msgs[i]->msg_style) {
		case PAM_PROMPT_ECHO_OFF:
		case PAM_PROMPT_ECHO_ON:
			replies[i].resp = strdup(password != NULL ? password : "");
			if (replies[i].resp == NULL) {
				int j;

				for (j = 0; j < i; j++)
					free(replies[j].resp);
				free(replies);
				return PAM_BUF_ERR;
			}
			break;
		case PAM_ERROR_MSG:
			g_message("pam: %s", msgs[i]->msg);
			break;
		case PAM_TEXT_INFO:
			g_debug("pam: %s", msgs[i]->msg);
			break;
		default:
			break;
		}
	}
	*resp = replies;
	return PAM_SUCCESS;
}

static gpointer
auth_thread(gpointer data)
{
	GowlLock *self = (GowlLock *)data;
	struct pam_conv conv;
	pam_handle_t *pamh = NULL;
	const gchar *user;
	gchar answer;
	int rc;

	conv.conv = conversation;
	conv.appdata_ptr = self->auth_password;

	user = g_get_user_name();
	rc = pam_start(self->pam_service, user, &conv, &pamh);
	if (rc == PAM_SUCCESS) {
		rc = pam_authenticate(pamh, 0);
		if (rc == PAM_SUCCESS)
			/* An expired password or a disabled account is not an
			 * authenticated session, and skipping this is how a lock
			 * screen lets one through. */
			rc = pam_acct_mgmt(pamh, 0);
	} else {
		g_warning("pam_start(%s): %s", self->pam_service,
		          pam_strerror(NULL, rc));
	}
	if (pamh != NULL)
		pam_end(pamh, rc);

	/* The copy this thread was given goes now, wherever it got to. */
	if (self->auth_password != NULL) {
		explicit_bzero(self->auth_password,
		               strlen(self->auth_password));
		g_free(self->auth_password);
		self->auth_password = NULL;
	}

	answer = rc == PAM_SUCCESS ? 'y' : 'n';
	{
		ssize_t w = write(self->auth_pipe[1], &answer, 1);

		(void)w;
	}
	return NULL;
}

void
gowl_lock_auth_start(GowlLock *self)
{
	if (self->phase == GOWL_LOCK_AUTHENTICATING)
		return;
	if (self->password_len == 0) {
		/* Nothing typed: say so rather than making PAM wait a second to
		 * refuse an empty string. */
		self->phase = GOWL_LOCK_IDLE;
		return;
	}

	/* A previous attempt's thread has finished (the pipe answered before
	 * the phase left AUTHENTICATING), but it has not been joined. */
	gowl_lock_auth_join(self);

	self->auth_password = g_strndup(self->password, self->password_len);
	gowl_lock_password_clear(self);
	self->phase = GOWL_LOCK_AUTHENTICATING;

	self->auth_thread = g_thread_new("gowl-lock-pam", auth_thread, self);
}

void
gowl_lock_auth_join(GowlLock *self)
{
	if (self->auth_thread != NULL) {
		g_thread_join(self->auth_thread);
		self->auth_thread = NULL;
	}
}

void
gowl_lock_auth_result(GowlLock *self, gboolean ok)
{
	gowl_lock_auth_join(self);

	if (ok) {
		gowl_lock_password_clear(self);
		gowl_lock_unlock_and_exit(self);
		return;
	}

	self->failures++;
	self->phase = GOWL_LOCK_FAILED;
	gowl_lock_password_clear(self);
	gowl_lock_damage_all(self);
}
