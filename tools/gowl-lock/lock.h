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

/**
 * gowl-lock:
 *
 * The screen lock, as a program of its own -- what i3lock is to i3 and
 * swaylock is to sway.
 *
 * gowl had a lock built into the compositor as a module.  That works,
 * and it is what this falls back to, but it puts two things in the
 * compositor's address space that should not be there: the password,
 * and PAM, which dlopens whatever the system's stack names into the
 * process that calls it.  Under `cmacs --gowl' that process is the
 * editor -- with an Elisp evaluator, an MCP server and a D-Bus
 * interface attached to it -- and a PAM module that crashes takes the
 * whole desktop with it.
 *
 * Speaking ext-session-lock-v1 instead buys the property that makes the
 * split worth making: the compositor, not this program, owns the locked
 * state.  If this program crashes the screen stays locked behind the
 * compositor's own opaque backdrop, and gowl starts it again.  There is
 * no window in which a dead lock screen is an open desktop.
 *
 * Structure:
 *   main.c         options, --help / --license, the run loop
 *   lock-wayland.c the protocol: registry, outputs, surfaces, input
 *   lock-render.c  cairo: the backdrop and the password indicator
 *   lock-pam.c     authentication, on a thread, answering over a pipe
 */

#ifndef GOWL_LOCK_H
#define GOWL_LOCK_H

#include <glib.h>
#include <stdint.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

#include "ext-session-lock-v1-client-protocol.h"

G_BEGIN_DECLS

/* How the password field looks while it is being typed into. */
typedef enum {
	GOWL_LOCK_IDLE,          /* waiting for a password */
	GOWL_LOCK_TYPING,        /* one or more characters entered */
	GOWL_LOCK_AUTHENTICATING,/* PAM is thinking */
	GOWL_LOCK_FAILED         /* PAM said no */
} GowlLockPhase;

typedef struct _GowlLock GowlLock;

/**
 * GowlLockOutput:
 *
 * One screen.  Created for every wl_output, including ones that appear
 * after the session is already locked -- the protocol requires a surface
 * on every output, and a display plugged in mid-lock that never got one
 * would be a protocol error as well as a hole in the lock.
 */
typedef struct {
	GowlLock                    *lock;
	struct wl_output            *output;
	guint32                      name;      /* registry name */
	struct wl_surface           *surface;
	struct ext_session_lock_surface_v1 *lock_surface;
	struct wl_callback          *frame;
	gint                         width;     /* logical, from configure */
	gint                         height;
	gint                         scale;     /* integer buffer scale */
	gboolean                     configured;
	gboolean                     dirty;
	struct wl_list               link;
} GowlLockOutput;

/**
 * GowlLock:
 *
 * Everything the lock screen is: the connection, the globals it needs,
 * one entry per screen, the password being typed, and how it should
 * look.
 */
struct _GowlLock {
	/* Wayland */
	struct wl_display           *display;
	struct wl_registry          *registry;
	struct wl_compositor        *compositor;
	struct wl_shm               *shm;
	struct wl_seat              *seat;
	struct wl_keyboard          *keyboard;
	struct ext_session_lock_manager_v1 *lock_manager;
	struct ext_session_lock_v1  *session_lock;
	struct wl_list               outputs;   /* GowlLockOutput */

	/* xkb, for turning keycodes into characters */
	struct xkb_context          *xkb;
	struct xkb_keymap           *keymap;
	struct xkb_state            *xkb_state;
	gint32                       repeat_rate;
	gint32                       repeat_delay;

	/* State */
	gboolean                     locked;    /* the compositor said so */
	gboolean                     finished;  /* ... or refused */
	gboolean                     running;
	GowlLockPhase                phase;
	gchar                       *password;  /* NUL-terminated, wiped */
	gsize                        password_len;
	gsize                        password_alloc;
	gint                         failures;

	/* PAM, on its own thread, answering down a pipe */
	GThread                     *auth_thread;
	gint                         auth_pipe[2];
	gchar                       *auth_password;   /* handed to the thread */

	/* Options */
	gchar                       *pam_service;
	gchar                       *image_path;
	gchar                       *image_mode;
	gchar                       *font;
	gdouble                      font_size;
	gdouble                      bg[4];
	gdouble                      fg[4];
	gdouble                      accent[4];
	gdouble                      error[4];
	gboolean                     show_failures;
	gint                         ready_fd;

	/* The backdrop, decoded once and scaled per screen. */
	gpointer                     image;     /* GdkPixbuf* */
};

/* --- lock-wayland.c --- */
gboolean gowl_lock_connect        (GowlLock *self, GError **error);
void     gowl_lock_engage         (GowlLock *self);
void     gowl_lock_disconnect     (GowlLock *self);
void     gowl_lock_damage_all     (GowlLock *self);
void     gowl_lock_unlock_and_exit(GowlLock *self);

/* --- lock-render.c --- */
void     gowl_lock_render_output  (GowlLock *self, GowlLockOutput *out);
void     gowl_lock_load_image     (GowlLock *self);
void     gowl_lock_free_image     (GowlLock *self);

/* --- lock-pam.c --- */
void     gowl_lock_auth_start     (GowlLock *self);
void     gowl_lock_auth_result    (GowlLock *self, gboolean ok);
void     gowl_lock_auth_join      (GowlLock *self);

/* --- shared --- */
void     gowl_lock_password_clear (GowlLock *self);

G_END_DECLS

#endif /* GOWL_LOCK_H */
