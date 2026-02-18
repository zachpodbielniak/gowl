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
 * GowlModuleScreenlock:
 *
 * A built-in screen lock module that renders lock surfaces using cairo,
 * authenticates passwords via PAM, and supports auto-lock on idle
 * timeout.  Each monitor gets its own full-screen lock surface on the
 * BLOCK scene layer.
 *
 * Lock surface rendering:
 *   - Solid background colour (configurable, default Catppuccin Mocha base)
 *   - Centered circle indicator showing password entry state
 *   - Dot indicators for each character entered
 *   - Status text ("Enter Password", "Authenticating...", "Wrong Password")
 *
 * Configuration (YAML):
 *   modules:
 *     screenlock:
 *       enabled: true
 *       pam-service: "gowl"
 *       auto-lock-timeout: 300
 *       bg-color: "#1e1e2e"
 *       text-color: "#cdd6f4"
 *       indicator-color: "#89b4fa"
 *       error-color: "#f38ba8"
 *       font: "monospace"
 *       font-size: 24
 */

#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "gowl-screenlock"

#include <glib-object.h>
#include <gmodule.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <cairo/cairo.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#include <drm_fourcc.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/types/wlr_scene.h>

#include <security/pam_appl.h>

#include "gowl-enums.h"
#include "module/gowl-module.h"
#include "interfaces/gowl-startup-handler.h"
#include "interfaces/gowl-shutdown-handler.h"
#include "interfaces/gowl-lock-handler.h"
#include "core/gowl-compositor.h"
#include "core/gowl-monitor.h"
#include "module/gowl-module-manager.h"

/* ----------------------------------------------------------------
 * Custom wlr_buffer for cairo-rendered pixel data
 * ---------------------------------------------------------------- */

/**
 * ScreenlockBuffer:
 *
 * A wlr_buffer subtype that wraps a g_malloc'd pixel buffer in
 * DRM_FORMAT_ARGB8888.  Cairo renders to this buffer, and wlroots
 * reads the pixel data via begin_data_ptr_access.
 */
typedef struct {
	struct wlr_buffer base;
	guchar *pixels;
	gsize   size;
	gint    stride;
} ScreenlockBuffer;

static void
screenlock_buffer_destroy(struct wlr_buffer *buf)
{
	ScreenlockBuffer *self;

	self = wl_container_of(buf, self, base);
	g_free(self->pixels);
	g_free(self);
}

static bool
screenlock_buffer_begin_data_ptr_access(
	struct wlr_buffer *buf,
	uint32_t           flags,
	void             **data,
	uint32_t          *format,
	size_t            *stride
){
	ScreenlockBuffer *self;

	(void)flags;
	self = wl_container_of(buf, self, base);
	*data   = (void *)self->pixels;
	*format = DRM_FORMAT_ARGB8888;
	*stride = (size_t)self->stride;
	return true;
}

static void
screenlock_buffer_end_data_ptr_access(struct wlr_buffer *buf)
{
	(void)buf;
}

static const struct wlr_buffer_impl screenlock_buffer_impl = {
	.destroy              = screenlock_buffer_destroy,
	.begin_data_ptr_access = screenlock_buffer_begin_data_ptr_access,
	.end_data_ptr_access  = screenlock_buffer_end_data_ptr_access,
};

/* ----------------------------------------------------------------
 * Per-monitor lock surface state
 * ---------------------------------------------------------------- */

typedef struct {
	struct wlr_scene_buffer *scene_buf;
	gint width;
	gint height;
} LockSurface;

static void
lock_surface_free(gpointer data)
{
	LockSurface *ls;

	ls = (LockSurface *)data;
	if (ls == NULL)
		return;
	if (ls->scene_buf != NULL)
		wlr_scene_node_destroy(&ls->scene_buf->node);
	g_free(ls);
}

/* ----------------------------------------------------------------
 * PAM authentication data (passed to auth thread)
 * ---------------------------------------------------------------- */

typedef struct {
	gchar    *password;      /* copied from module, zeroed after use */
	gchar    *username;
	gchar    *pam_service;
	gboolean  success;
	gint      write_fd;      /* pipe write end for result notification */
} AuthData;

/* ----------------------------------------------------------------
 * Module type declaration
 * ---------------------------------------------------------------- */

#define GOWL_TYPE_MODULE_SCREENLOCK (gowl_module_screenlock_get_type())
G_DECLARE_FINAL_TYPE(GowlModuleScreenlock, gowl_module_screenlock,
                     GOWL, MODULE_SCREENLOCK, GowlModule)

struct _GowlModuleScreenlock {
	GowlModule  parent_instance;

	/* Lock state */
	gboolean    is_locked;
	gchar      *password_buf;
	gsize       password_len;
	gsize       password_alloc;
	gboolean    auth_failed;
	gboolean    authenticating;
	gint        failed_attempts;

	/* Config */
	gchar      *pam_service;
	gint        auto_lock_timeout;
	gdouble     bg_color[4];
	gdouble     text_color[4];
	gdouble     indicator_color[4];
	gdouble     error_color[4];
	gchar      *font_face;
	gdouble     font_size;

	/* Per-monitor state */
	GHashTable *surfaces;

	/* References */
	gpointer    compositor;
	struct wl_event_source *idle_timer;
	gint        auth_pipe[2];
	struct wl_event_source *auth_fd_source;
};

/* Forward declarations */
static void screenlock_startup_init   (GowlStartupHandlerInterface *iface);
static void screenlock_shutdown_init  (GowlShutdownHandlerInterface *iface);
static void screenlock_lock_init      (GowlLockHandlerInterface *iface);

G_DEFINE_TYPE_WITH_CODE(GowlModuleScreenlock, gowl_module_screenlock,
	GOWL_TYPE_MODULE,
	G_IMPLEMENT_INTERFACE(GOWL_TYPE_STARTUP_HANDLER,
		screenlock_startup_init)
	G_IMPLEMENT_INTERFACE(GOWL_TYPE_SHUTDOWN_HANDLER,
		screenlock_shutdown_init)
	G_IMPLEMENT_INTERFACE(GOWL_TYPE_LOCK_HANDLER,
		screenlock_lock_init))

/* ----------------------------------------------------------------
 * Colour parsing helper
 * ---------------------------------------------------------------- */

/**
 * parse_hex_color:
 * @hex: a hex colour string like "#rrggbb" or "#rrggbbaa"
 * @rgba: (out): array of 4 doubles (R, G, B, A) in [0.0, 1.0]
 *
 * Parses a hex colour string into normalised RGBA components.
 */
static void
parse_hex_color(
	const gchar *hex,
	gdouble     *rgba
){
	guint r, g, b, a;

	r = g = b = 0;
	a = 255;

	if (hex == NULL || hex[0] != '#') {
		rgba[0] = rgba[1] = rgba[2] = 0.0;
		rgba[3] = 1.0;
		return;
	}

	if (strlen(hex) == 7) {
		sscanf(hex + 1, "%02x%02x%02x", &r, &g, &b);
	} else if (strlen(hex) == 9) {
		sscanf(hex + 1, "%02x%02x%02x%02x", &r, &g, &b, &a);
	}

	rgba[0] = (gdouble)r / 255.0;
	rgba[1] = (gdouble)g / 255.0;
	rgba[2] = (gdouble)b / 255.0;
	rgba[3] = (gdouble)a / 255.0;
}

/* ----------------------------------------------------------------
 * Lock surface rendering (cairo)
 * ---------------------------------------------------------------- */

/**
 * render_lock_surface:
 * @self: the screenlock module
 * @width: surface width in pixels
 * @height: surface height in pixels
 *
 * Renders a lock screen image using cairo.  Shows a background colour,
 * centered indicator circle, password dots, and status text.
 *
 * Returns: (transfer full): a new ScreenlockBuffer, or %NULL on error
 */
static ScreenlockBuffer *
render_lock_surface(
	GowlModuleScreenlock *self,
	gint                  width,
	gint                  height
){
	cairo_surface_t *cs;
	cairo_t *cr;
	ScreenlockBuffer *buf;
	guchar *pixels;
	gint stride;
	gdouble cx, cy;
	gdouble radius;
	gdouble dot_radius;
	gsize i;
	const gchar *status_text;
	gdouble *ind_color;

	/* Create cairo image surface (ARGB32 matches DRM_FORMAT_ARGB8888) */
	cs = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
	if (cairo_surface_status(cs) != CAIRO_STATUS_SUCCESS) {
		g_warning("Failed to create cairo surface %dx%d", width, height);
		cairo_surface_destroy(cs);
		return NULL;
	}

	cr = cairo_create(cs);

	/* Background */
	cairo_set_source_rgba(cr, self->bg_color[0], self->bg_color[1],
	                      self->bg_color[2], self->bg_color[3]);
	cairo_paint(cr);

	/* Center of screen */
	cx = (gdouble)width / 2.0;
	cy = (gdouble)height / 2.0;
	radius = 80.0;

	/* Select indicator colour based on state */
	if (self->auth_failed) {
		ind_color = self->error_color;
	} else if (self->authenticating) {
		ind_color = self->indicator_color;
	} else {
		ind_color = self->indicator_color;
	}

	/* Indicator circle outline */
	cairo_set_source_rgba(cr, ind_color[0], ind_color[1],
	                      ind_color[2], ind_color[3]);
	cairo_set_line_width(cr, 4.0);
	cairo_arc(cr, cx, cy, radius, 0, 2 * G_PI);
	cairo_stroke(cr);

	/* Password dots inside the circle */
	if (self->password_len > 0) {
		gdouble total_width;
		gdouble start_x;

		dot_radius = 6.0;
		total_width = (gdouble)self->password_len * dot_radius * 3.0
		              - dot_radius;

		/* Clamp to fit inside circle */
		if (total_width > radius * 1.4)
			total_width = radius * 1.4;

		start_x = cx - total_width / 2.0;

		cairo_set_source_rgba(cr, ind_color[0], ind_color[1],
		                      ind_color[2], ind_color[3]);
		for (i = 0; i < self->password_len; i++) {
			gdouble dx;

			if (self->password_len == 1) {
				dx = cx;
			} else {
				dx = start_x + (gdouble)i *
				     (total_width / (gdouble)(self->password_len - 1));
			}
			cairo_arc(cr, dx, cy, dot_radius, 0, 2 * G_PI);
			cairo_fill(cr);
		}
	}

	/* Status text below the circle */
	if (self->authenticating) {
		status_text = "Authenticating...";
	} else if (self->auth_failed) {
		status_text = "Wrong Password";
	} else if (self->password_len == 0) {
		status_text = "Enter Password";
	} else {
		status_text = NULL;
	}

	if (status_text != NULL) {
		cairo_text_extents_t extents;

		cairo_select_font_face(cr, self->font_face,
		                       CAIRO_FONT_SLANT_NORMAL,
		                       CAIRO_FONT_WEIGHT_NORMAL);
		cairo_set_font_size(cr, self->font_size);
		cairo_text_extents(cr, status_text, &extents);

		cairo_set_source_rgba(cr, self->text_color[0],
		                      self->text_color[1],
		                      self->text_color[2],
		                      self->text_color[3]);
		cairo_move_to(cr,
		              cx - extents.width / 2.0 - extents.x_bearing,
		              cy + radius + 40.0);
		cairo_show_text(cr, status_text);
	}

	/* Failed attempts counter */
	if (self->failed_attempts > 0) {
		g_autofree gchar *attempts_text = NULL;
		cairo_text_extents_t extents;

		attempts_text = g_strdup_printf("%d failed attempt%s",
			self->failed_attempts,
			self->failed_attempts == 1 ? "" : "s");

		cairo_select_font_face(cr, self->font_face,
		                       CAIRO_FONT_SLANT_NORMAL,
		                       CAIRO_FONT_WEIGHT_NORMAL);
		cairo_set_font_size(cr, self->font_size * 0.7);
		cairo_text_extents(cr, attempts_text, &extents);

		cairo_set_source_rgba(cr, self->error_color[0],
		                      self->error_color[1],
		                      self->error_color[2],
		                      self->error_color[3] * 0.8);
		cairo_move_to(cr,
		              cx - extents.width / 2.0 - extents.x_bearing,
		              cy + radius + 70.0);
		cairo_show_text(cr, attempts_text);
	}

	cairo_destroy(cr);
	cairo_surface_flush(cs);

	/* Copy pixel data to a wlr_buffer */
	stride = cairo_image_surface_get_stride(cs);
	pixels = (guchar *)g_malloc((gsize)stride * (gsize)height);
	memcpy(pixels, cairo_image_surface_get_data(cs),
	       (gsize)stride * (gsize)height);
	cairo_surface_destroy(cs);

	buf = (ScreenlockBuffer *)g_malloc0(sizeof(ScreenlockBuffer));
	wlr_buffer_init(&buf->base, &screenlock_buffer_impl, width, height);
	buf->pixels = pixels;
	buf->size   = (gsize)stride * (gsize)height;
	buf->stride = stride;

	return buf;
}

/**
 * screenlock_update_surface:
 * @self: the screenlock module
 * @name: monitor name
 * @ls: per-monitor lock surface state
 *
 * Re-renders the lock surface for a single monitor and updates the
 * scene buffer with the new pixel data.
 */
static void
screenlock_update_surface(
	GowlModuleScreenlock *self,
	const gchar          *name,
	LockSurface          *ls
){
	ScreenlockBuffer *buf;

	buf = render_lock_surface(self, ls->width, ls->height);
	if (buf == NULL) {
		g_warning("Failed to render lock surface for monitor '%s'", name);
		return;
	}

	wlr_scene_buffer_set_buffer(ls->scene_buf, &buf->base);
	wlr_buffer_drop(&buf->base);
}

/**
 * screenlock_redraw_all:
 * @self: the screenlock module
 *
 * Redraws lock surfaces on all monitors.
 */
static void
screenlock_redraw_all(GowlModuleScreenlock *self)
{
	GHashTableIter iter;
	gpointer key, value;

	g_hash_table_iter_init(&iter, self->surfaces);
	while (g_hash_table_iter_next(&iter, &key, &value)) {
		screenlock_update_surface(self, (const gchar *)key,
		                          (LockSurface *)value);
	}
}

/* ----------------------------------------------------------------
 * PAM authentication (threaded)
 * ---------------------------------------------------------------- */

/**
 * pam_conversation:
 *
 * PAM conversation callback.  Returns the stored password for
 * PAM_PROMPT_ECHO_OFF messages.
 */
static int
pam_conversation(
	int                        num_msg,
	const struct pam_message **msg,
	struct pam_response      **resp,
	void                      *appdata
){
	struct pam_response *reply;
	const gchar *password;
	int i;

	password = (const gchar *)appdata;
	reply = (struct pam_response *)calloc(
		(size_t)num_msg, sizeof(struct pam_response));
	if (reply == NULL)
		return PAM_CONV_ERR;

	for (i = 0; i < num_msg; i++) {
		if (msg[i]->msg_style == PAM_PROMPT_ECHO_OFF ||
		    msg[i]->msg_style == PAM_PROMPT_ECHO_ON) {
			reply[i].resp = strdup(password);
			if (reply[i].resp == NULL) {
				free(reply);
				return PAM_CONV_ERR;
			}
		}
	}

	*resp = reply;
	return PAM_SUCCESS;
}

/**
 * pam_auth_thread:
 * @data: AuthData pointer
 *
 * Runs PAM authentication in a separate thread to avoid blocking
 * the compositor event loop.  Writes a single byte to the pipe
 * on completion (1 = success, 0 = failure).
 */
static gpointer
pam_auth_thread(gpointer data)
{
	AuthData *auth;
	pam_handle_t *pamh;
	struct pam_conv conv;
	int ret;
	guchar result;

	auth = (AuthData *)data;

	conv.conv = pam_conversation;
	conv.appdata_ptr = (void *)auth->password;

	pamh = NULL;
	ret = pam_start(auth->pam_service, auth->username, &conv, &pamh);

	if (ret == PAM_SUCCESS)
		ret = pam_authenticate(pamh, 0);
	if (ret == PAM_SUCCESS)
		ret = pam_acct_mgmt(pamh, 0);

	if (pamh != NULL)
		pam_end(pamh, ret);

	auth->success = (ret == PAM_SUCCESS);
	result = auth->success ? 1 : 0;

	/* Zero the password copy */
	if (auth->password != NULL) {
		explicit_bzero(auth->password, strlen(auth->password));
		g_free(auth->password);
		auth->password = NULL;
	}

	/* Signal the main event loop */
	if (write(auth->write_fd, &result, 1) < 0)
		g_warning("Failed to write PAM result to pipe");

	g_free(auth->username);
	g_free(auth->pam_service);
	g_free(auth);

	return NULL;
}

/**
 * on_pam_result:
 *
 * Event loop callback when the PAM thread writes its result.
 * Reads the result byte and either unlocks or shows error.
 */
static int
on_pam_result(int fd, uint32_t mask, void *data)
{
	GowlModuleScreenlock *self;
	guchar result;
	ssize_t n;

	(void)mask;
	self = (GowlModuleScreenlock *)data;

	n = read(fd, &result, 1);
	if (n != 1) {
		g_warning("Failed to read PAM result from pipe");
		self->authenticating = FALSE;
		self->auth_failed = TRUE;
		screenlock_redraw_all(self);
		return 0;
	}

	self->authenticating = FALSE;

	if (result == 1) {
		/* Authentication succeeded -- unlock */
		self->auth_failed = FALSE;
		self->failed_attempts = 0;
		self->is_locked = FALSE;

		/* Destroy all lock surfaces */
		g_hash_table_remove_all(self->surfaces);

		/* Tell compositor to unlock */
		gowl_compositor_set_locked(
			(GowlCompositor *)self->compositor, FALSE);

		g_info("Screen unlocked");
	} else {
		/* Authentication failed */
		self->auth_failed = TRUE;
		self->failed_attempts++;

		/* Clear password buffer */
		if (self->password_buf != NULL) {
			explicit_bzero(self->password_buf,
			               self->password_alloc);
			self->password_len = 0;
		}

		screenlock_redraw_all(self);
		g_info("Authentication failed (attempt %d)",
		       self->failed_attempts);
	}

	return 0;
}

/**
 * screenlock_start_auth:
 * @self: the screenlock module
 *
 * Starts PAM authentication in a background thread.
 */
static void
screenlock_start_auth(GowlModuleScreenlock *self)
{
	AuthData *auth;
	const gchar *user;

	if (self->authenticating)
		return;

	if (self->password_len == 0)
		return;

	self->authenticating = TRUE;
	screenlock_redraw_all(self);

	/* Build auth data with a copy of the password */
	user = g_get_user_name();

	auth = (AuthData *)g_malloc0(sizeof(AuthData));
	auth->password = g_strndup(self->password_buf, self->password_len);
	auth->username = g_strdup(user);
	auth->pam_service = g_strdup(self->pam_service);
	auth->write_fd = self->auth_pipe[1];

	/* Zero the module's password buffer */
	explicit_bzero(self->password_buf, self->password_alloc);
	self->password_len = 0;

	/* Spawn the auth thread */
	g_thread_unref(g_thread_new("pam-auth", pam_auth_thread, auth));
}

/* ----------------------------------------------------------------
 * Password buffer management
 * ---------------------------------------------------------------- */

/**
 * screenlock_append_char:
 * @self: the screenlock module
 * @codepoint: Unicode codepoint to append
 *
 * Appends a UTF-8 encoded character to the password buffer,
 * growing it if needed.
 */
static void
screenlock_append_char(
	GowlModuleScreenlock *self,
	guint32               codepoint
){
	gchar utf8[6];
	gint len;

	len = g_unichar_to_utf8((gunichar)codepoint, utf8);
	if (len <= 0)
		return;

	/* Grow buffer if needed */
	while (self->password_len + (gsize)len + 1 > self->password_alloc) {
		gsize new_alloc;
		gchar *new_buf;

		new_alloc = self->password_alloc * 2;
		if (new_alloc < 128)
			new_alloc = 128;

		new_buf = (gchar *)g_malloc0(new_alloc);
		if (self->password_buf != NULL) {
			memcpy(new_buf, self->password_buf, self->password_len);
			explicit_bzero(self->password_buf,
			               self->password_alloc);
			g_free(self->password_buf);
		}
		self->password_buf = new_buf;
		self->password_alloc = new_alloc;
	}

	memcpy(self->password_buf + self->password_len, utf8, (gsize)len);
	self->password_len += (gsize)len;
	self->password_buf[self->password_len] = '\0';
}

/* ----------------------------------------------------------------
 * Auto-lock idle timer
 * ---------------------------------------------------------------- */

/**
 * on_idle_lock_timeout:
 *
 * Timer callback fired when the auto-lock timeout elapses.
 * Triggers the lock via the module manager.
 */
static int
on_idle_lock_timeout(void *data)
{
	GowlModuleScreenlock *self;
	GowlModuleManager *mgr;

	self = (GowlModuleScreenlock *)data;

	if (self->is_locked)
		return 0;

	mgr = gowl_compositor_get_module_manager(
		(GowlCompositor *)self->compositor);
	if (mgr != NULL)
		gowl_module_manager_dispatch_lock(mgr, self->compositor);

	return 0;
}

/* ----------------------------------------------------------------
 * GowlLockHandler interface implementation
 * ---------------------------------------------------------------- */

/**
 * screenlock_on_lock:
 *
 * Creates lock surfaces on all monitors and engages the compositor
 * lock state.
 */
static void
screenlock_on_lock(
	GowlLockHandler *handler,
	gpointer         compositor
){
	GowlModuleScreenlock *self;
	GowlCompositor *comp;
	GList *monitors;
	GList *l;

	self = GOWL_MODULE_SCREENLOCK(handler);
	comp = (GowlCompositor *)compositor;

	if (self->is_locked)
		return;

	self->is_locked = TRUE;
	self->auth_failed = FALSE;
	self->authenticating = FALSE;

	/* Clear any leftover password */
	if (self->password_buf != NULL) {
		explicit_bzero(self->password_buf, self->password_alloc);
		self->password_len = 0;
	}

	/* Tell the compositor to enter locked state */
	gowl_compositor_set_locked(comp, TRUE);

	/* Create lock surfaces for each monitor */
	monitors = gowl_compositor_get_monitors(comp);
	for (l = monitors; l != NULL; l = l->next) {
		GowlMonitor *mon;
		const gchar *name;
		gint x, y, w, h;
		struct wlr_scene_tree *block_layer;
		struct wlr_scene_buffer *sbuf;
		ScreenlockBuffer *buf;
		LockSurface *ls;

		mon = (GowlMonitor *)l->data;
		name = gowl_monitor_get_name(mon);
		gowl_monitor_get_geometry(mon, &x, &y, &w, &h);

		if (w <= 0 || h <= 0)
			continue;

		/* Render the lock surface */
		buf = render_lock_surface(self, w, h);
		if (buf == NULL)
			continue;

		/* Create scene buffer on the BLOCK layer */
		block_layer = gowl_compositor_get_scene_layer(
			comp, GOWL_SCENE_LAYER_BLOCK);
		sbuf = wlr_scene_buffer_create(block_layer, &buf->base);
		wlr_buffer_drop(&buf->base);

		wlr_scene_node_set_position(&sbuf->node, x, y);

		/* Store per-monitor state */
		ls = (LockSurface *)g_malloc0(sizeof(LockSurface));
		ls->scene_buf = sbuf;
		ls->width = w;
		ls->height = h;

		g_hash_table_replace(self->surfaces,
		                     g_strdup(name), (gpointer)ls);

		g_debug("Lock surface created for monitor '%s' (%dx%d)",
		        name, w, h);
	}

	g_info("Screen locked on %u monitor(s)",
	       g_hash_table_size(self->surfaces));
}

/**
 * screenlock_on_unlock:
 *
 * Destroys all lock surfaces and disengages the lock state.
 */
static void
screenlock_on_unlock(
	GowlLockHandler *handler,
	gpointer         compositor
){
	GowlModuleScreenlock *self;

	self = GOWL_MODULE_SCREENLOCK(handler);

	if (!self->is_locked)
		return;

	self->is_locked = FALSE;
	self->auth_failed = FALSE;
	self->authenticating = FALSE;
	self->failed_attempts = 0;

	/* Clear password buffer */
	if (self->password_buf != NULL) {
		explicit_bzero(self->password_buf, self->password_alloc);
		self->password_len = 0;
	}

	/* Destroy all lock surfaces */
	g_hash_table_remove_all(self->surfaces);

	/* Tell the compositor to unlock */
	gowl_compositor_set_locked(
		(GowlCompositor *)compositor, FALSE);

	/* Reset the idle timer */
	if (self->idle_timer != NULL && self->auto_lock_timeout > 0) {
		wl_event_source_timer_update(self->idle_timer,
			self->auto_lock_timeout * 1000);
	}

	g_info("Screen unlocked");
}

/**
 * screenlock_on_key_input:
 *
 * Handles key events during lock state for password entry.
 */
static gboolean
screenlock_on_key_input(
	GowlLockHandler *handler,
	guint            keysym,
	guint32          codepoint,
	gboolean         pressed
){
	GowlModuleScreenlock *self;

	self = GOWL_MODULE_SCREENLOCK(handler);

	if (!self->is_locked)
		return FALSE;

	/* Only process key press events */
	if (!pressed)
		return TRUE;

	/* Ignore input while authenticating */
	if (self->authenticating)
		return TRUE;

	/* Clear error state on any keypress after failure */
	if (self->auth_failed) {
		self->auth_failed = FALSE;
	}

	switch (keysym) {
	case XKB_KEY_Return:
	case XKB_KEY_KP_Enter:
		screenlock_start_auth(self);
		break;

	case XKB_KEY_BackSpace:
		if (self->password_len > 0) {
			/* Remove last UTF-8 character */
			const gchar *prev;
			prev = g_utf8_find_prev_char(
				self->password_buf,
				self->password_buf + self->password_len);
			if (prev != NULL) {
				gsize new_len;
				new_len = (gsize)(prev - self->password_buf);
				/* Zero removed bytes */
				explicit_bzero(
					self->password_buf + new_len,
					self->password_len - new_len);
				self->password_len = new_len;
			}
		}
		screenlock_redraw_all(self);
		break;

	case XKB_KEY_Escape:
		/* Clear the entire password buffer */
		if (self->password_len > 0) {
			explicit_bzero(self->password_buf,
			               self->password_alloc);
			self->password_len = 0;
		}
		self->auth_failed = FALSE;
		screenlock_redraw_all(self);
		break;

	default:
		/* Append printable character */
		if (codepoint >= 0x20) {
			screenlock_append_char(self, codepoint);
			screenlock_redraw_all(self);
		}
		break;
	}

	return TRUE;
}

/**
 * screenlock_on_output:
 *
 * Handles monitor add/geometry change during lock.
 * Creates or updates the lock surface for the monitor.
 */
static void
screenlock_on_output(
	GowlLockHandler *handler,
	gpointer         compositor,
	gpointer         monitor
){
	GowlModuleScreenlock *self;
	GowlCompositor *comp;
	GowlMonitor *mon;
	const gchar *name;
	gint x, y, w, h;
	struct wlr_scene_tree *block_layer;
	struct wlr_scene_buffer *sbuf;
	ScreenlockBuffer *buf;
	LockSurface *ls;

	self = GOWL_MODULE_SCREENLOCK(handler);
	comp = (GowlCompositor *)compositor;
	mon = (GowlMonitor *)monitor;

	if (!self->is_locked)
		return;

	name = gowl_monitor_get_name(mon);
	gowl_monitor_get_geometry(mon, &x, &y, &w, &h);

	if (w <= 0 || h <= 0)
		return;

	/* Remove existing surface for this monitor if any */
	g_hash_table_remove(self->surfaces, name);

	/* Create new lock surface */
	buf = render_lock_surface(self, w, h);
	if (buf == NULL)
		return;

	block_layer = gowl_compositor_get_scene_layer(
		comp, GOWL_SCENE_LAYER_BLOCK);
	sbuf = wlr_scene_buffer_create(block_layer, &buf->base);
	wlr_buffer_drop(&buf->base);

	wlr_scene_node_set_position(&sbuf->node, x, y);

	ls = (LockSurface *)g_malloc0(sizeof(LockSurface));
	ls->scene_buf = sbuf;
	ls->width = w;
	ls->height = h;

	g_hash_table_replace(self->surfaces,
	                     g_strdup(name), (gpointer)ls);

	g_debug("Lock surface updated for monitor '%s' (%dx%d)", name, w, h);
}

/**
 * screenlock_on_output_destroy:
 *
 * Removes the lock surface for a destroyed monitor.
 */
static void
screenlock_on_output_destroy(
	GowlLockHandler *handler,
	gpointer         monitor
){
	GowlModuleScreenlock *self;
	GowlMonitor *mon;
	const gchar *name;

	self = GOWL_MODULE_SCREENLOCK(handler);
	mon = (GowlMonitor *)monitor;

	if (!self->is_locked)
		return;

	name = gowl_monitor_get_name(mon);
	g_hash_table_remove(self->surfaces, name);

	g_debug("Lock surface removed for monitor '%s'", name);
}

/**
 * screenlock_on_activity:
 *
 * Resets the idle auto-lock timer on user activity.
 */
static void
screenlock_on_activity(GowlLockHandler *handler)
{
	GowlModuleScreenlock *self;

	self = GOWL_MODULE_SCREENLOCK(handler);

	if (self->idle_timer != NULL && self->auto_lock_timeout > 0) {
		wl_event_source_timer_update(self->idle_timer,
			self->auto_lock_timeout * 1000);
	}
}

/* ----------------------------------------------------------------
 * Interface init functions
 * ---------------------------------------------------------------- */

static void
screenlock_lock_init(GowlLockHandlerInterface *iface)
{
	iface->on_lock           = screenlock_on_lock;
	iface->on_unlock         = screenlock_on_unlock;
	iface->on_key_input      = screenlock_on_key_input;
	iface->on_output         = screenlock_on_output;
	iface->on_output_destroy = screenlock_on_output_destroy;
	iface->on_activity       = screenlock_on_activity;
}

/* ----------------------------------------------------------------
 * GowlStartupHandler implementation
 * ---------------------------------------------------------------- */

static void
screenlock_on_startup(
	GowlStartupHandler *handler,
	gpointer             compositor
){
	GowlModuleScreenlock *self;
	GowlCompositor *comp;
	struct wl_event_loop *loop;

	self = GOWL_MODULE_SCREENLOCK(handler);
	comp = (GowlCompositor *)compositor;

	self->compositor = compositor;

	/* Set up PAM result pipe */
	if (pipe(self->auth_pipe) == 0) {
		loop = gowl_compositor_get_event_loop(comp);
		if (loop != NULL) {
			self->auth_fd_source = wl_event_loop_add_fd(
				loop, self->auth_pipe[0],
				WL_EVENT_READABLE,
				on_pam_result, self);
		}
	} else {
		g_warning("Failed to create auth pipe");
		self->auth_pipe[0] = -1;
		self->auth_pipe[1] = -1;
	}

	/* Set up auto-lock timer */
	if (self->auto_lock_timeout > 0) {
		loop = gowl_compositor_get_event_loop(comp);
		if (loop != NULL) {
			self->idle_timer = wl_event_loop_add_timer(
				loop, on_idle_lock_timeout, self);
			wl_event_source_timer_update(self->idle_timer,
				self->auto_lock_timeout * 1000);
			g_info("Auto-lock timer set to %d seconds",
			       self->auto_lock_timeout);
		}
	}

	g_info("Screenlock module started (pam-service=%s, timeout=%ds)",
	       self->pam_service, self->auto_lock_timeout);
}

static void
screenlock_startup_init(GowlStartupHandlerInterface *iface)
{
	iface->on_startup = screenlock_on_startup;
}

/* ----------------------------------------------------------------
 * GowlShutdownHandler implementation
 * ---------------------------------------------------------------- */

static void
screenlock_on_shutdown(
	GowlShutdownHandler *handler,
	gpointer             compositor
){
	GowlModuleScreenlock *self;

	(void)compositor;
	self = GOWL_MODULE_SCREENLOCK(handler);

	/* Remove idle timer */
	if (self->idle_timer != NULL) {
		wl_event_source_remove(self->idle_timer);
		self->idle_timer = NULL;
	}

	/* Remove auth fd source */
	if (self->auth_fd_source != NULL) {
		wl_event_source_remove(self->auth_fd_source);
		self->auth_fd_source = NULL;
	}

	/* Close auth pipe */
	if (self->auth_pipe[0] >= 0)
		close(self->auth_pipe[0]);
	if (self->auth_pipe[1] >= 0)
		close(self->auth_pipe[1]);

	/* Clean up lock surfaces */
	g_hash_table_remove_all(self->surfaces);

	g_info("Screenlock module shut down");
}

static void
screenlock_shutdown_init(GowlShutdownHandlerInterface *iface)
{
	iface->on_shutdown = screenlock_on_shutdown;
}

/* ----------------------------------------------------------------
 * GowlModule virtual methods
 * ---------------------------------------------------------------- */

static gboolean
screenlock_activate(GowlModule *mod)
{
	(void)mod;
	return TRUE;
}

static void
screenlock_deactivate(GowlModule *mod)
{
	(void)mod;
}

static const gchar *
screenlock_get_name(GowlModule *mod)
{
	(void)mod;
	return "screenlock";
}

static const gchar *
screenlock_get_description(GowlModule *mod)
{
	(void)mod;
	return "Built-in screen lock with PAM authentication";
}

static const gchar *
screenlock_get_version(GowlModule *mod)
{
	(void)mod;
	return "1.0.0";
}

/**
 * screenlock_configure:
 *
 * Receives per-module settings from the YAML config and applies them.
 */
static void
screenlock_configure(
	GowlModule *mod,
	gpointer    config
){
	GowlModuleScreenlock *self;
	GHashTable *cfg;
	const gchar *val;

	self = GOWL_MODULE_SCREENLOCK(mod);
	cfg = (GHashTable *)config;

	if (cfg == NULL)
		return;

	val = (const gchar *)g_hash_table_lookup(cfg, "pam-service");
	if (val != NULL) {
		g_free(self->pam_service);
		self->pam_service = g_strdup(val);
	}

	val = (const gchar *)g_hash_table_lookup(cfg, "auto-lock-timeout");
	if (val != NULL)
		self->auto_lock_timeout = atoi(val);

	val = (const gchar *)g_hash_table_lookup(cfg, "bg-color");
	if (val != NULL)
		parse_hex_color(val, self->bg_color);

	val = (const gchar *)g_hash_table_lookup(cfg, "text-color");
	if (val != NULL)
		parse_hex_color(val, self->text_color);

	val = (const gchar *)g_hash_table_lookup(cfg, "indicator-color");
	if (val != NULL)
		parse_hex_color(val, self->indicator_color);

	val = (const gchar *)g_hash_table_lookup(cfg, "error-color");
	if (val != NULL)
		parse_hex_color(val, self->error_color);

	val = (const gchar *)g_hash_table_lookup(cfg, "font");
	if (val != NULL) {
		g_free(self->font_face);
		self->font_face = g_strdup(val);
	}

	val = (const gchar *)g_hash_table_lookup(cfg, "font-size");
	if (val != NULL) {
		gdouble sz;
		sz = g_ascii_strtod(val, NULL);
		if (sz > 0.0)
			self->font_size = sz;
	}
}

/* ----------------------------------------------------------------
 * GObject lifecycle
 * ---------------------------------------------------------------- */

static void
gowl_module_screenlock_finalize(GObject *object)
{
	GowlModuleScreenlock *self;

	self = GOWL_MODULE_SCREENLOCK(object);

	/* Zero and free password buffer */
	if (self->password_buf != NULL) {
		explicit_bzero(self->password_buf, self->password_alloc);
		g_free(self->password_buf);
	}

	g_free(self->pam_service);
	g_free(self->font_face);
	g_clear_pointer(&self->surfaces, g_hash_table_unref);

	G_OBJECT_CLASS(gowl_module_screenlock_parent_class)->finalize(object);
}

static void
gowl_module_screenlock_class_init(GowlModuleScreenlockClass *klass)
{
	GObjectClass *object_class;
	GowlModuleClass *mod_class;

	object_class = G_OBJECT_CLASS(klass);
	mod_class = GOWL_MODULE_CLASS(klass);

	object_class->finalize = gowl_module_screenlock_finalize;

	mod_class->activate       = screenlock_activate;
	mod_class->deactivate     = screenlock_deactivate;
	mod_class->get_name       = screenlock_get_name;
	mod_class->get_description = screenlock_get_description;
	mod_class->get_version    = screenlock_get_version;
	mod_class->configure      = screenlock_configure;
}

static void
gowl_module_screenlock_init(GowlModuleScreenlock *self)
{
	self->is_locked = FALSE;
	self->password_buf = NULL;
	self->password_len = 0;
	self->password_alloc = 0;
	self->auth_failed = FALSE;
	self->authenticating = FALSE;
	self->failed_attempts = 0;

	/* Default config */
	self->pam_service = g_strdup("gowl");
	self->auto_lock_timeout = 300;
	self->font_face = g_strdup("monospace");
	self->font_size = 24.0;

	/* Catppuccin Mocha defaults */
	parse_hex_color("#1e1e2e", self->bg_color);
	parse_hex_color("#cdd6f4", self->text_color);
	parse_hex_color("#89b4fa", self->indicator_color);
	parse_hex_color("#f38ba8", self->error_color);

	self->surfaces = g_hash_table_new_full(
		g_str_hash, g_str_equal,
		g_free, lock_surface_free);

	self->compositor = NULL;
	self->idle_timer = NULL;
	self->auth_pipe[0] = -1;
	self->auth_pipe[1] = -1;
	self->auth_fd_source = NULL;
}

/* ----------------------------------------------------------------
 * Module entry point
 * ---------------------------------------------------------------- */

G_MODULE_EXPORT GType
gowl_module_register(void)
{
	return GOWL_TYPE_MODULE_SCREENLOCK;
}
