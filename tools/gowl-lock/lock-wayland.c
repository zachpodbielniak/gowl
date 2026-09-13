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
 * The protocol half: binding the globals, taking the lock, and keeping
 * exactly one lock surface per screen for as long as the session is
 * locked -- including screens plugged in and unplugged while it is.
 */

#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "gowl-lock"

#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <glib/gstdio.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#include "lock.h"

/* ------------------------------------------------------------------
 * Lock surfaces
 * ------------------------------------------------------------------ */

static void
output_render_if_ready(GowlLockOutput *out)
{
	if (!out->configured || out->width <= 0 || out->height <= 0)
		return;
	gowl_lock_render_output(out->lock, out);
	out->dirty = FALSE;
}

/*
 * The compositor has told us the size of this screen.
 *
 * The protocol is strict here: the surface must be committed with a
 * buffer of exactly this size, after acking, and it must be committed
 * before anything else -- a lock surface that never draws is a hole in
 * the lock, which is why wlroots treats a wrong-sized commit as a
 * protocol error rather than a hint.
 */
static void
lock_surface_configure(void *data,
                       struct ext_session_lock_surface_v1 *surface,
                       uint32_t serial, uint32_t width, uint32_t height)
{
	GowlLockOutput *out = (GowlLockOutput *)data;

	ext_session_lock_surface_v1_ack_configure(surface, serial);
	out->width  = (gint)width;
	out->height = (gint)height;
	out->configured = TRUE;
	output_render_if_ready(out);
}

static const struct ext_session_lock_surface_v1_listener lock_surface_listener = {
	.configure = lock_surface_configure,
};

/* Put a lock surface on one screen.  Idempotent: a screen that already
 * has one is left alone, which is what makes it safe to call this both
 * when the lock is granted and when an output appears afterwards. */
static void
output_lock(GowlLock *self, GowlLockOutput *out)
{
	if (self->session_lock == NULL || out->lock_surface != NULL)
		return;
	if (self->compositor == NULL)
		return;

	out->surface = wl_compositor_create_surface(self->compositor);
	if (out->surface == NULL)
		return;
	if (out->scale > 1)
		wl_surface_set_buffer_scale(out->surface, out->scale);

	out->lock_surface = ext_session_lock_v1_get_lock_surface(
		self->session_lock, out->surface, out->output);
	ext_session_lock_surface_v1_add_listener(out->lock_surface,
		&lock_surface_listener, out);
}

static void
output_unlock(GowlLockOutput *out)
{
	g_clear_pointer(&out->frame, wl_callback_destroy);
	g_clear_pointer(&out->lock_surface,
	                ext_session_lock_surface_v1_destroy);
	g_clear_pointer(&out->surface, wl_surface_destroy);
	out->configured = FALSE;
}

/* ------------------------------------------------------------------
 * Outputs
 * ------------------------------------------------------------------ */

static void
output_geometry(void *data, struct wl_output *o, int32_t x, int32_t y,
                int32_t pw, int32_t ph, int32_t subpixel, const char *make,
                const char *model, int32_t transform)
{
	(void)data; (void)o; (void)x; (void)y; (void)pw; (void)ph;
	(void)subpixel; (void)make; (void)model; (void)transform;
}

static void
output_mode(void *data, struct wl_output *o, uint32_t flags, int32_t w,
            int32_t h, int32_t refresh)
{
	(void)data; (void)o; (void)flags; (void)w; (void)h; (void)refresh;
}

/* The compositor's configure carries the logical size, so the mode is
 * not interesting -- but the scale is: it decides how many pixels the
 * buffer needs, and getting it wrong is a lock screen rendered at half
 * resolution on exactly the laptop panels that have a high one. */
static void
output_scale(void *data, struct wl_output *o, int32_t factor)
{
	GowlLockOutput *out = (GowlLockOutput *)data;

	(void)o;
	if (factor < 1)
		factor = 1;
	if (out->scale == factor)
		return;
	out->scale = factor;
	if (out->surface != NULL)
		wl_surface_set_buffer_scale(out->surface, factor);
	out->dirty = TRUE;
}

static void
output_done(void *data, struct wl_output *o)
{
	GowlLockOutput *out = (GowlLockOutput *)data;

	(void)o;
	if (out->dirty)
		output_render_if_ready(out);
}

static void
output_name(void *data, struct wl_output *o, const char *name)
{
	(void)data; (void)o; (void)name;
}

static void
output_description(void *data, struct wl_output *o, const char *desc)
{
	(void)data; (void)o; (void)desc;
}

static const struct wl_output_listener output_listener = {
	.geometry    = output_geometry,
	.mode        = output_mode,
	.done        = output_done,
	.scale       = output_scale,
	.name        = output_name,
	.description = output_description,
};

static void
output_free(GowlLockOutput *out)
{
	output_unlock(out);
	if (out->output != NULL)
		wl_output_release(out->output);
	wl_list_remove(&out->link);
	g_free(out);
}

/* ------------------------------------------------------------------
 * Keyboard
 * ------------------------------------------------------------------ */

static void
password_append(GowlLock *self, guint32 codepoint)
{
	gchar utf8[8];
	gint len;

	/* A password field is not a text editor: a bound stops a stuck key
	 * from growing the buffer without limit, and the limit is far above
	 * anything anybody types. */
	if (self->password_len > 1024)
		return;

	len = g_unichar_to_utf8((gunichar)codepoint, utf8);
	if (len <= 0)
		return;

	if (self->password_len + (gsize)len + 1 > self->password_alloc) {
		gsize want = self->password_alloc * 2;
		gchar *grown;

		if (want < 256)
			want = 256;
		grown = (gchar *)g_malloc0(want);
		if (self->password != NULL) {
			memcpy(grown, self->password, self->password_len);
			/* The old allocation held the password; it must not be
			 * left in freed memory for whatever allocates it next. */
			explicit_bzero(self->password, self->password_alloc);
			g_free(self->password);
		}
		self->password = grown;
		self->password_alloc = want;
	}

	memcpy(self->password + self->password_len, utf8, (gsize)len);
	self->password_len += (gsize)len;
	self->password[self->password_len] = '\0';
}

void
gowl_lock_password_clear(GowlLock *self)
{
	if (self->password != NULL)
		explicit_bzero(self->password, self->password_alloc);
	self->password_len = 0;
}

static void
kb_keymap(void *data, struct wl_keyboard *kb, uint32_t format, int32_t fd,
          uint32_t size)
{
	GowlLock *self = (GowlLock *)data;
	gchar *map;

	(void)kb;
	if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
		close(fd);
		return;
	}
	map = (gchar *)mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
	if (map == MAP_FAILED) {
		close(fd);
		return;
	}
	if (self->xkb_state != NULL)
		xkb_state_unref(self->xkb_state);
	if (self->keymap != NULL)
		xkb_keymap_unref(self->keymap);
	self->keymap = xkb_keymap_new_from_string(self->xkb, map,
		XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
	self->xkb_state = self->keymap != NULL
		? xkb_state_new(self->keymap) : NULL;
	munmap(map, size);
	close(fd);
}

static void
kb_enter(void *data, struct wl_keyboard *kb, uint32_t serial,
         struct wl_surface *surface, struct wl_array *keys)
{
	(void)data; (void)kb; (void)serial; (void)surface; (void)keys;
}

static void
kb_leave(void *data, struct wl_keyboard *kb, uint32_t serial,
         struct wl_surface *surface)
{
	(void)data; (void)kb; (void)serial; (void)surface;
}

static void
kb_key(void *data, struct wl_keyboard *kb, uint32_t serial, uint32_t time,
       uint32_t key, uint32_t state)
{
	GowlLock *self = (GowlLock *)data;
	xkb_keysym_t sym;
	guint32 codepoint;

	(void)kb; (void)serial; (void)time;
	if (state != WL_KEYBOARD_KEY_STATE_PRESSED || self->xkb_state == NULL)
		return;
	/* PAM is answering; anything typed now would race the reply. */
	if (self->phase == GOWL_LOCK_AUTHENTICATING)
		return;

	sym = xkb_state_key_get_one_sym(self->xkb_state, key + 8);
	codepoint = xkb_state_key_get_utf32(self->xkb_state, key + 8);

	/* Any key clears a previous refusal, so the field stops shouting
	 * the moment the person starts again. */
	if (self->phase == GOWL_LOCK_FAILED)
		self->phase = self->password_len > 0
			? GOWL_LOCK_TYPING : GOWL_LOCK_IDLE;

	switch (sym) {
	case XKB_KEY_Return:
	case XKB_KEY_KP_Enter:
		gowl_lock_auth_start(self);
		break;
	case XKB_KEY_BackSpace:
		if (self->password_len > 0) {
			const gchar *prev = g_utf8_find_prev_char(
				self->password, self->password + self->password_len);

			if (prev != NULL) {
				gsize len = (gsize)(prev - self->password);

				explicit_bzero(self->password + len,
				               self->password_len - len);
				self->password_len = len;
			}
		}
		if (self->password_len == 0)
			self->phase = GOWL_LOCK_IDLE;
		break;
	case XKB_KEY_Escape:
		gowl_lock_password_clear(self);
		self->phase = GOWL_LOCK_IDLE;
		break;
	case XKB_KEY_u:
		/* Ctrl+u clears the field, as it does in a shell. */
		if (xkb_state_mod_name_is_active(self->xkb_state, XKB_MOD_NAME_CTRL,
		                                  XKB_STATE_MODS_EFFECTIVE) > 0) {
			gowl_lock_password_clear(self);
			self->phase = GOWL_LOCK_IDLE;
			break;
		}
		/* fall through */
	default:
		if (codepoint >= 0x20 && codepoint != 0x7f) {
			password_append(self, codepoint);
			self->phase = GOWL_LOCK_TYPING;
		}
		break;
	}
	gowl_lock_damage_all(self);
}

static void
kb_modifiers(void *data, struct wl_keyboard *kb, uint32_t serial,
             uint32_t depressed, uint32_t latched, uint32_t locked,
             uint32_t group)
{
	GowlLock *self = (GowlLock *)data;

	(void)kb; (void)serial;
	if (self->xkb_state != NULL)
		xkb_state_update_mask(self->xkb_state, depressed, latched, locked,
		                      0, 0, group);
}

static void
kb_repeat_info(void *data, struct wl_keyboard *kb, int32_t rate,
               int32_t delay)
{
	GowlLock *self = (GowlLock *)data;

	(void)kb;
	self->repeat_rate = rate;
	self->repeat_delay = delay;
}

static const struct wl_keyboard_listener keyboard_listener = {
	.keymap      = kb_keymap,
	.enter       = kb_enter,
	.leave       = kb_leave,
	.key         = kb_key,
	.modifiers   = kb_modifiers,
	.repeat_info = kb_repeat_info,
};

static void
seat_capabilities(void *data, struct wl_seat *seat, uint32_t caps)
{
	GowlLock *self = (GowlLock *)data;

	if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) != 0
	    && self->keyboard == NULL) {
		self->keyboard = wl_seat_get_keyboard(seat);
		wl_keyboard_add_listener(self->keyboard, &keyboard_listener, self);
	} else if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) == 0
	           && self->keyboard != NULL) {
		wl_keyboard_release(self->keyboard);
		self->keyboard = NULL;
	}
}

static void
seat_name(void *data, struct wl_seat *seat, const char *name)
{
	(void)data; (void)seat; (void)name;
}

static const struct wl_seat_listener seat_listener = {
	.capabilities = seat_capabilities,
	.name         = seat_name,
};

/* ------------------------------------------------------------------
 * The lock object
 * ------------------------------------------------------------------ */

/* The compositor confirms that nothing of the desktop is visible any
 * more.  Only now is it safe to tell whoever is waiting (a suspend
 * that is holding an inhibitor for exactly this) that the screen is
 * sealed. */
static void
lock_locked(void *data, struct ext_session_lock_v1 *lock)
{
	GowlLock *self = (GowlLock *)data;

	(void)lock;
	self->locked = TRUE;
	g_debug("the session is locked");

	if (self->ready_fd >= 0) {
		ssize_t w = write(self->ready_fd, "\n", 1);

		(void)w;
		close(self->ready_fd);
		self->ready_fd = -1;
	}
}

/*
 * Refused, or superseded: another lock client already holds the session,
 * or the compositor decided this one may not.
 *
 * Exiting is the only correct response -- the session is somebody else's
 * lock now -- and it must be a FAILURE exit, because whatever started
 * this (a suspend, a keybind) has to be able to tell "locked" from "did
 * not lock".
 */
static void
lock_finished(void *data, struct ext_session_lock_v1 *lock)
{
	GowlLock *self = (GowlLock *)data;

	(void)lock;
	g_warning("the compositor refused the lock (another lock screen is "
	          "already up?)");
	self->finished = TRUE;
	self->running = FALSE;
}

static const struct ext_session_lock_v1_listener lock_listener = {
	.locked   = lock_locked,
	.finished = lock_finished,
};

/* ------------------------------------------------------------------
 * Registry
 * ------------------------------------------------------------------ */

static void
registry_global(void *data, struct wl_registry *registry, uint32_t name,
                const char *interface, uint32_t version)
{
	GowlLock *self = (GowlLock *)data;

	if (g_strcmp0(interface, wl_compositor_interface.name) == 0) {
		self->compositor = (struct wl_compositor *)wl_registry_bind(
			registry, name, &wl_compositor_interface,
			MIN(version, 4u));
	} else if (g_strcmp0(interface, wl_shm_interface.name) == 0) {
		self->shm = (struct wl_shm *)wl_registry_bind(
			registry, name, &wl_shm_interface, 1);
	} else if (g_strcmp0(interface, wl_seat_interface.name) == 0
	           && self->seat == NULL) {
		self->seat = (struct wl_seat *)wl_registry_bind(
			registry, name, &wl_seat_interface, MIN(version, 7u));
		wl_seat_add_listener(self->seat, &seat_listener, self);
	} else if (g_strcmp0(interface,
	                     ext_session_lock_manager_v1_interface.name) == 0) {
		self->lock_manager =
			(struct ext_session_lock_manager_v1 *)wl_registry_bind(
				registry, name,
				&ext_session_lock_manager_v1_interface, 1);
	} else if (g_strcmp0(interface, wl_output_interface.name) == 0) {
		GowlLockOutput *out = g_new0(GowlLockOutput, 1);

		out->lock  = self;
		out->name  = name;
		out->scale = 1;
		out->output = (struct wl_output *)wl_registry_bind(
			registry, name, &wl_output_interface, MIN(version, 4u));
		wl_output_add_listener(out->output, &output_listener, out);
		wl_list_insert(&self->outputs, &out->link);

		/* A screen plugged in while the session is already locked
		 * needs a surface of its own immediately: the protocol
		 * requires one on every output, and a compositor that finds an
		 * output without one has to keep showing its own backdrop
		 * there -- a black rectangle where the lock screen should be. */
		if (self->session_lock != NULL)
			output_lock(self, out);
	}
}

/* A screen was unplugged.  Its surface goes with it; the ones left keep
 * theirs, and the compositor re-configures them at their new sizes. */
static void
registry_global_remove(void *data, struct wl_registry *registry,
                       uint32_t name)
{
	GowlLock *self = (GowlLock *)data;
	GowlLockOutput *out, *tmp;

	(void)registry;
	wl_list_for_each_safe(out, tmp, &self->outputs, link) {
		if (out->name == name) {
			g_debug("a screen was unplugged");
			output_free(out);
			return;
		}
	}
}

static const struct wl_registry_listener registry_listener = {
	.global        = registry_global,
	.global_remove = registry_global_remove,
};

/* ------------------------------------------------------------------
 * Public
 * ------------------------------------------------------------------ */

gboolean
gowl_lock_connect(GowlLock *self, GError **error)
{
	wl_list_init(&self->outputs);

	self->display = wl_display_connect(NULL);
	if (self->display == NULL) {
		g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_FAILED,
		            "cannot reach a Wayland compositor (is "
		            "WAYLAND_DISPLAY set?)");
		return FALSE;
	}

	self->xkb = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	self->registry = wl_display_get_registry(self->display);
	wl_registry_add_listener(self->registry, &registry_listener, self);
	/* Twice: the first round brings in the globals, the second the
	 * events they send on binding -- the seat's capabilities, and each
	 * output's scale. */
	wl_display_roundtrip(self->display);
	wl_display_roundtrip(self->display);

	if (self->compositor == NULL || self->shm == NULL) {
		g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_FAILED,
		            "the compositor offers no wl_compositor/wl_shm");
		return FALSE;
	}
	if (self->lock_manager == NULL) {
		g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_FAILED,
		            "this compositor does not support "
		            "ext-session-lock-v1");
		return FALSE;
	}
	return TRUE;
}

void
gowl_lock_engage(GowlLock *self)
{
	GowlLockOutput *out;

	self->session_lock = ext_session_lock_manager_v1_lock(
		self->lock_manager);
	ext_session_lock_v1_add_listener(self->session_lock, &lock_listener,
	                                  self);

	wl_list_for_each(out, &self->outputs, link)
		output_lock(self, out);

	self->running = TRUE;
}

/* Every screen redraws: the password field changed on all of them at
 * once, because there is one field and several copies of it. */
void
gowl_lock_damage_all(GowlLock *self)
{
	GowlLockOutput *out;

	wl_list_for_each(out, &self->outputs, link)
		output_render_if_ready(out);
}

/*
 * The password was accepted.
 *
 * unlock_and_destroy() is the request that opens the session, and the
 * order matters: it has to reach the compositor before this process
 * exits, or the compositor sees the connection drop with the lock still
 * held -- which it correctly reads as "the lock program died" and keeps
 * the screen locked.  Hence the explicit roundtrip.
 */
void
gowl_lock_unlock_and_exit(GowlLock *self)
{
	if (self->session_lock != NULL) {
		ext_session_lock_v1_unlock_and_destroy(self->session_lock);
		self->session_lock = NULL;
		wl_display_roundtrip(self->display);
	}
	self->running = FALSE;
}

void
gowl_lock_disconnect(GowlLock *self)
{
	GowlLockOutput *out, *tmp;

	wl_list_for_each_safe(out, tmp, &self->outputs, link)
		output_free(out);

	g_clear_pointer(&self->session_lock, ext_session_lock_v1_destroy);
	g_clear_pointer(&self->lock_manager,
	                ext_session_lock_manager_v1_destroy);
	if (self->keyboard != NULL) {
		wl_keyboard_release(self->keyboard);
		self->keyboard = NULL;
	}
	if (self->seat != NULL) {
		wl_seat_release(self->seat);
		self->seat = NULL;
	}
	g_clear_pointer(&self->shm, wl_shm_destroy);
	g_clear_pointer(&self->compositor, wl_compositor_destroy);
	g_clear_pointer(&self->registry, wl_registry_destroy);
	if (self->xkb_state != NULL) {
		xkb_state_unref(self->xkb_state);
		self->xkb_state = NULL;
	}
	if (self->keymap != NULL) {
		xkb_keymap_unref(self->keymap);
		self->keymap = NULL;
	}
	if (self->xkb != NULL) {
		xkb_context_unref(self->xkb);
		self->xkb = NULL;
	}
	if (self->display != NULL) {
		wl_display_disconnect(self->display);
		self->display = NULL;
	}
}
