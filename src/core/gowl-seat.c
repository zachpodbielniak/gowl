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

#include "gowl-core-private.h"

#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>

/**
 * GowlSeat:
 *
 * Represents the Wayland seat (input device group).  Holds a reference
 * to the focused client, the keyboard group, and cursor state.
 * The struct definition lives in gowl-core-private.h.
 */

G_DEFINE_FINAL_TYPE(GowlSeat, gowl_seat, G_TYPE_OBJECT)

/* Signal identifiers */
enum {
	SIGNAL_FOCUS_CHANGED,
	SIGNAL_CLIPBOARD_CHANGED,
	SIGNAL_PRIMARY_SELECTION_CHANGED,
	SIGNAL_FOCUS_REDIRECTED,
	SIGNAL_FOCUS_RESTORED,
	N_SIGNALS
};

static guint seat_signals[N_SIGNALS] = { 0, };

/* --- GObject lifecycle --- */

static void
gowl_seat_dispose(GObject *object)
{
	GowlSeat *self;

	self = GOWL_SEAT(object);

	self->focused_client  = NULL;
	self->keyboard_group  = NULL;
	self->cursor          = NULL;

	G_OBJECT_CLASS(gowl_seat_parent_class)->dispose(object);
}

static void
gowl_seat_finalize(GObject *object)
{
	G_OBJECT_CLASS(gowl_seat_parent_class)->finalize(object);
}

/* --- class / instance init --- */

static void
gowl_seat_class_init(GowlSeatClass *klass)
{
	GObjectClass *object_class;

	object_class = G_OBJECT_CLASS(klass);

	object_class->dispose  = gowl_seat_dispose;
	object_class->finalize = gowl_seat_finalize;

	/**
	 * GowlSeat::focus-changed:
	 * @seat: the #GowlSeat that emitted the signal
	 *
	 * Emitted when the focused client changes.
	 */
	seat_signals[SIGNAL_FOCUS_CHANGED] =
		g_signal_new("focus-changed",
		             G_TYPE_FROM_CLASS(klass),
		             G_SIGNAL_RUN_LAST,
		             0,
		             NULL, NULL,
		             NULL,
		             G_TYPE_NONE,
		             0);

	/**
	 * GowlSeat::clipboard-changed:
	 * @seat: the #GowlSeat that emitted the signal
	 *
	 * Emitted when a Wayland client changes the clipboard selection.
	 * Listeners can call gowl_seat_get_clipboard() to read the
	 * new content.
	 */
	seat_signals[SIGNAL_CLIPBOARD_CHANGED] =
		g_signal_new("clipboard-changed",
		             G_TYPE_FROM_CLASS(klass),
		             G_SIGNAL_RUN_LAST,
		             0,
		             NULL, NULL,
		             NULL,
		             G_TYPE_NONE,
		             0);

	/**
	 * GowlSeat::primary-selection-changed:
	 * @seat: the #GowlSeat that emitted the signal
	 *
	 * Emitted when a Wayland client changes the primary selection.
	 * Listeners can call gowl_seat_get_primary_selection() to read
	 * the new content.
	 */
	seat_signals[SIGNAL_PRIMARY_SELECTION_CHANGED] =
		g_signal_new("primary-selection-changed",
		             G_TYPE_FROM_CLASS(klass),
		             G_SIGNAL_RUN_LAST,
		             0,
		             NULL, NULL,
		             NULL,
		             G_TYPE_NONE,
		             0);

	/**
	 * GowlSeat::focus-redirected:
	 * @seat: the #GowlSeat that emitted the signal
	 * @from: (nullable): the client that lost focus (a
	 *   #GowlClient, passed as a bare pointer)
	 * @to: (nullable): the client that received focus
	 * @reason: why the redirect was initiated
	 *
	 * Emitted inside #gowl_seat_push_focus_redirect immediately
	 * after the new focus target takes effect.  Integration layers
	 * (e.g. cmacs `--gowl`) observe this signal to record the
	 * active token and decide when to pop.
	 */
	seat_signals[SIGNAL_FOCUS_REDIRECTED] =
		g_signal_new("focus-redirected",
		             G_TYPE_FROM_CLASS(klass),
		             G_SIGNAL_RUN_LAST,
		             0,
		             NULL, NULL,
		             NULL,
		             G_TYPE_NONE,
		             3,
		             G_TYPE_POINTER,
		             G_TYPE_POINTER,
		             GOWL_TYPE_FOCUS_REASON);

	/**
	 * GowlSeat::focus-restored:
	 * @seat: the #GowlSeat that emitted the signal
	 * @reason: the #GowlFocusReason that was recorded on push
	 *
	 * Emitted inside #gowl_seat_pop_focus_redirect after the saved
	 * focus has been re-applied.
	 */
	seat_signals[SIGNAL_FOCUS_RESTORED] =
		g_signal_new("focus-restored",
		             G_TYPE_FROM_CLASS(klass),
		             G_SIGNAL_RUN_LAST,
		             0,
		             NULL, NULL,
		             NULL,
		             G_TYPE_NONE,
		             1,
		             GOWL_TYPE_FOCUS_REASON);
}

static void
gowl_seat_init(GowlSeat *self)
{
	self->wlr_seat        = NULL;
	self->focused_client  = NULL;
	self->keyboard_group  = NULL;
	self->cursor          = NULL;
}

/* --- Public API --- */

/**
 * gowl_seat_new:
 *
 * Creates a new #GowlSeat.
 *
 * Returns: (transfer full): a newly allocated #GowlSeat
 */
GowlSeat *
gowl_seat_new(void)
{
	return (GowlSeat *)g_object_new(GOWL_TYPE_SEAT, NULL);
}

/**
 * gowl_seat_get_focused_client:
 * @self: a #GowlSeat
 *
 * Returns the client that currently holds keyboard focus.
 *
 * Returns: (transfer none) (nullable): the focused #GowlClient, or %NULL
 */
gpointer
gowl_seat_get_focused_client(GowlSeat *self)
{
	g_return_val_if_fail(GOWL_IS_SEAT(self), NULL);

	return self->focused_client;
}

/**
 * gowl_seat_set_focused_client:
 * @self: a #GowlSeat
 * @client: (nullable): the #GowlClient to focus, or %NULL to unfocus
 *
 * Sets the focused client and emits "focus-changed" if it changed.
 */
void
gowl_seat_set_focused_client(
	GowlSeat *self,
	gpointer  client
){
	g_return_if_fail(GOWL_IS_SEAT(self));

	if (self->focused_client != client) {
		self->focused_client = client;
		g_signal_emit(self, seat_signals[SIGNAL_FOCUS_CHANGED], 0);
	}
}

/* --- Input injection --- */

/**
 * gowl_seat_send_key:
 * @self: a #GowlSeat
 * @keycode: the XKB keycode to inject
 * @pressed: %TRUE for key press, %FALSE for key release
 *
 * Injects a synthetic key event to the focused surface.
 */
void
gowl_seat_send_key(
	GowlSeat *self,
	guint32   keycode,
	gboolean  pressed
){
	struct wlr_seat *seat;

	g_return_if_fail(GOWL_IS_SEAT(self));

	seat = (struct wlr_seat *)self->wlr_seat;
	if (seat == NULL)
		return;

	wlr_seat_keyboard_notify_key(seat,
		(guint32)(g_get_monotonic_time() / 1000),
		keycode,
		pressed ? WL_KEYBOARD_KEY_STATE_PRESSED
		        : WL_KEYBOARD_KEY_STATE_RELEASED);
}

/**
 * gowl_seat_send_text:
 * @self: a #GowlSeat
 * @text: UTF-8 string to type
 *
 * Injects synthetic key events to type @text character by character
 * on the focused surface.  For each character, looks up the XKB
 * keycode that produces it.  Shifted characters (level 1) are
 * wrapped in Shift press/release.  Characters that cannot be mapped
 * are silently skipped.
 */
void
gowl_seat_send_text(
	GowlSeat    *self,
	const gchar *text
){
	struct wlr_seat *seat;
	GowlKeyboardGroup *kb;
	struct wlr_keyboard_group *grp;
	struct xkb_keymap *keymap;
	xkb_keycode_t min_kc, max_kc;
	const gchar *p;

	g_return_if_fail(GOWL_IS_SEAT(self));
	g_return_if_fail(text != NULL);

	seat = (struct wlr_seat *)self->wlr_seat;
	kb   = (GowlKeyboardGroup *)self->keyboard_group;
	if (seat == NULL || kb == NULL)
		return;

	grp = (struct wlr_keyboard_group *)kb->wlr_group;
	if (grp == NULL)
		return;

	keymap = grp->keyboard.keymap;
	if (keymap == NULL)
		return;

	min_kc = xkb_keymap_min_keycode(keymap);
	max_kc = xkb_keymap_max_keycode(keymap);

	for (p = text; *p != '\0'; p = g_utf8_next_char(p)) {
		gunichar ch = g_utf8_get_char(p);
		xkb_keycode_t kc;
		xkb_keycode_t found_kc = 0;
		gint found_level = -1;
		guint32 ts, evdev_kc;

		/* Scan keycodes for one that produces this character. */
		for (kc = min_kc; kc <= max_kc && found_kc == 0; kc++) {
			int nlayouts, layout;

			nlayouts = xkb_keymap_num_layouts_for_key(keymap, kc);
			for (layout = 0; layout < nlayouts && found_kc == 0;
			     layout++) {
				int nlevels, level;

				nlevels = xkb_keymap_num_levels_for_key(
					keymap, kc, layout);
				/* Only check level 0 (plain) and 1 (shift). */
				if (nlevels > 2)
					nlevels = 2;

				for (level = 0; level < nlevels; level++) {
					const xkb_keysym_t *syms;
					int nsyms, s;

					nsyms = xkb_keymap_key_get_syms_by_level(
						keymap, kc, layout, level,
						&syms);
					for (s = 0; s < nsyms; s++) {
						if (xkb_keysym_to_utf32(
							syms[s]) == ch) {
							found_kc = kc;
							found_level = level;
							goto emit;
						}
					}
				}
			}
		}

	emit:
		if (found_kc == 0)
			continue;

		/* XKB keycodes are evdev + 8. */
		evdev_kc = found_kc - 8;
		ts = (guint32)(g_get_monotonic_time() / 1000);

		if (found_level == 1) {
			/* Shift press (evdev KEY_LEFTSHIFT = 42) */
			wlr_seat_keyboard_notify_key(seat, ts, 42,
				WL_KEYBOARD_KEY_STATE_PRESSED);
		}

		wlr_seat_keyboard_notify_key(seat, ts, evdev_kc,
			WL_KEYBOARD_KEY_STATE_PRESSED);
		ts = (guint32)(g_get_monotonic_time() / 1000);
		wlr_seat_keyboard_notify_key(seat, ts, evdev_kc,
			WL_KEYBOARD_KEY_STATE_RELEASED);

		if (found_level == 1) {
			ts = (guint32)(g_get_monotonic_time() / 1000);
			wlr_seat_keyboard_notify_key(seat, ts, 42,
				WL_KEYBOARD_KEY_STATE_RELEASED);
		}
	}
}

/**
 * gowl_seat_send_mouse_move:
 */
void
gowl_seat_send_mouse_move(
	GowlSeat *self,
	gdouble   x,
	gdouble   y
){
	g_return_if_fail(GOWL_IS_SEAT(self));

	/* Cursor movement requires the wlr_cursor, not the seat.
	 * This is dispatched through the GowlCursor wrapper. */
	if (self->cursor != NULL) {
		GowlCursor *cur = GOWL_CURSOR(self->cursor);
		struct wlr_cursor *wlr_cur;

		wlr_cur = (struct wlr_cursor *)cur->wlr_cursor;
		if (wlr_cur != NULL)
			wlr_cursor_warp_absolute(wlr_cur, NULL,
				x / 1.0, y / 1.0);
	}
}

/**
 * gowl_seat_send_mouse_button:
 */
void
gowl_seat_send_mouse_button(
	GowlSeat *self,
	guint32   button,
	gboolean  pressed
){
	struct wlr_seat *seat;

	g_return_if_fail(GOWL_IS_SEAT(self));

	seat = (struct wlr_seat *)self->wlr_seat;
	if (seat == NULL)
		return;

	wlr_seat_pointer_notify_button(seat,
		(guint32)(g_get_monotonic_time() / 1000),
		button,
		pressed ? WL_POINTER_BUTTON_STATE_PRESSED
		        : WL_POINTER_BUTTON_STATE_RELEASED);
}

/**
 * gowl_seat_send_scroll:
 */
void
gowl_seat_send_scroll(
	GowlSeat *self,
	gdouble   dx,
	gdouble   dy
){
	struct wlr_seat *seat;

	g_return_if_fail(GOWL_IS_SEAT(self));

	seat = (struct wlr_seat *)self->wlr_seat;
	if (seat == NULL)
		return;

	if (dy != 0.0)
		wlr_seat_pointer_notify_axis(seat,
			(guint32)(g_get_monotonic_time() / 1000),
			WL_POINTER_AXIS_VERTICAL_SCROLL,
			dy, (gint32)(dy * 120),
			WL_POINTER_AXIS_SOURCE_WHEEL,
			WL_POINTER_AXIS_RELATIVE_DIRECTION_IDENTICAL);
	if (dx != 0.0)
		wlr_seat_pointer_notify_axis(seat,
			(guint32)(g_get_monotonic_time() / 1000),
			WL_POINTER_AXIS_HORIZONTAL_SCROLL,
			dx, (gint32)(dx * 120),
			WL_POINTER_AXIS_SOURCE_WHEEL,
			WL_POINTER_AXIS_RELATIVE_DIRECTION_IDENTICAL);
}

/* --- Clipboard --- */

/*
 * Custom wlr_data_source that serves a text string.
 * Used by gowl_seat_set_clipboard().
 */
struct gowl_text_source {
	struct wlr_data_source base;
	gchar *text;
};

static void
gowl_text_source_send(
	struct wlr_data_source *source,
	const char             *mime_type,
	int32_t                 fd
){
	struct gowl_text_source *ts;

	ts = wl_container_of(source, ts, base);
	(void)mime_type;

	if (ts->text != NULL) {
		size_t len = strlen(ts->text);
		ssize_t written = 0;

		while ((size_t)written < len) {
			ssize_t n = write(fd, ts->text + written,
			                  len - (size_t)written);
			if (n <= 0)
				break;
			written += n;
		}
	}
	close(fd);
}

static void
gowl_text_source_destroy(struct wlr_data_source *source)
{
	struct gowl_text_source *ts;

	ts = wl_container_of(source, ts, base);
	g_free(ts->text);
	g_free(ts);
}

static const struct wlr_data_source_impl gowl_text_source_impl = {
	.send    = gowl_text_source_send,
	.destroy = gowl_text_source_destroy,
};

/*
 * Custom wlr_primary_selection_source that serves a text string.
 * Used by gowl_seat_set_primary_selection().
 */
struct gowl_primary_text_source {
	struct wlr_primary_selection_source base;
	gchar *text;
};

static void
gowl_primary_text_source_send(
	struct wlr_primary_selection_source *source,
	const char                          *mime_type,
	int                                  fd
){
	struct gowl_primary_text_source *ts;

	ts = wl_container_of(source, ts, base);
	(void)mime_type;

	if (ts->text != NULL) {
		size_t len = strlen(ts->text);
		ssize_t written = 0;

		while ((size_t)written < len) {
			ssize_t n = write(fd, ts->text + written,
			                  len - (size_t)written);
			if (n <= 0)
				break;
			written += n;
		}
	}
	close(fd);
}

static void
gowl_primary_text_source_destroy(
	struct wlr_primary_selection_source *source
){
	struct gowl_primary_text_source *ts;

	ts = wl_container_of(source, ts, base);
	g_free(ts->text);
	g_free(ts);
}

static const struct wlr_primary_selection_source_impl
gowl_primary_text_source_impl = {
	.send    = gowl_primary_text_source_send,
	.destroy = gowl_primary_text_source_destroy,
};

/*
 * Helper: read text from a data source by requesting "text/plain;charset=utf-8"
 * (falling back to "text/plain") through a pipe.  The send callback writes
 * directly to the fd for compositor-owned sources.
 */
static gchar *
read_text_from_source_pipe(int read_fd)
{
	GString *buf;
	gchar tmp[4096];
	ssize_t n;

	buf = g_string_new(NULL);
	for (;;) {
		n = read(read_fd, tmp, sizeof(tmp));
		if (n <= 0)
			break;
		g_string_append_len(buf, tmp, n);
	}
	close(read_fd);

	if (buf->len == 0) {
		g_string_free(buf, TRUE);
		return NULL;
	}
	return g_string_free(buf, FALSE);
}

static gboolean
mime_array_contains(const struct wl_array *arr, const char *mime)
{
	char **p;

	wl_array_for_each(p, arr) {
		if (g_strcmp0(*p, mime) == 0)
			return TRUE;
	}
	return FALSE;
}

/**
 * gowl_seat_get_clipboard:
 * @self: a #GowlSeat
 *
 * Reads the current clipboard content as text.  Works for
 * compositor-owned sources (including those set via
 * gowl_seat_set_clipboard).  For client-owned sources, the
 * read is synchronous through a pipe — this works for local
 * Wayland clients but may block briefly.
 *
 * Returns: (transfer full) (nullable): clipboard text, or %NULL
 */
gchar *
gowl_seat_get_clipboard(GowlSeat *self)
{
	struct wlr_seat *seat;
	struct wlr_data_source *source;
	const char *mime;
	int fds[2];

	g_return_val_if_fail(GOWL_IS_SEAT(self), NULL);

	seat = (struct wlr_seat *)self->wlr_seat;
	if (seat == NULL)
		return NULL;

	source = seat->selection_source;
	if (source == NULL)
		return NULL;

	/* Pick best MIME type. */
	if (mime_array_contains(&source->mime_types,
	                        "text/plain;charset=utf-8"))
		mime = "text/plain;charset=utf-8";
	else if (mime_array_contains(&source->mime_types,
	                             "text/plain"))
		mime = "text/plain";
	else if (mime_array_contains(&source->mime_types,
	                             "UTF8_STRING"))
		mime = "UTF8_STRING";
	else
		return NULL;

	if (pipe(fds) != 0)
		return NULL;

	wlr_data_source_send(source, mime, fds[1]);
	/* send callback closes fds[1] */

	return read_text_from_source_pipe(fds[0]);
}

/**
 * gowl_seat_set_clipboard:
 * @self: a #GowlSeat
 * @text: the UTF-8 text to place on the clipboard
 *
 * Sets the Wayland clipboard to @text by creating a compositor-
 * owned data source offering text/plain and UTF8_STRING.
 */
void
gowl_seat_set_clipboard(
	GowlSeat    *self,
	const gchar *text
){
	struct wlr_seat *seat;
	struct gowl_text_source *ts;
	char *mime_plain, *mime_utf8, *mime_utf8_str;

	g_return_if_fail(GOWL_IS_SEAT(self));
	g_return_if_fail(text != NULL);

	seat = (struct wlr_seat *)self->wlr_seat;
	if (seat == NULL)
		return;

	ts = g_new0(struct gowl_text_source, 1);
	ts->text = g_strdup(text);
	wlr_data_source_init(&ts->base, &gowl_text_source_impl);

	/* Offer MIME types. wl_array entries are char* pointers. */
	mime_plain = g_strdup("text/plain");
	mime_utf8  = g_strdup("text/plain;charset=utf-8");
	mime_utf8_str = g_strdup("UTF8_STRING");

	{
		char **slot;
		slot = wl_array_add(&ts->base.mime_types, sizeof(char *));
		*slot = mime_plain;
		slot = wl_array_add(&ts->base.mime_types, sizeof(char *));
		*slot = mime_utf8;
		slot = wl_array_add(&ts->base.mime_types, sizeof(char *));
		*slot = mime_utf8_str;
	}

	wlr_seat_set_selection(seat, &ts->base,
	                       wl_display_next_serial(seat->display));
}

/**
 * gowl_seat_get_primary_selection:
 * @self: a #GowlSeat
 *
 * Reads the current primary selection content as text.
 *
 * Returns: (transfer full) (nullable): selection text, or %NULL
 */
gchar *
gowl_seat_get_primary_selection(GowlSeat *self)
{
	struct wlr_seat *seat;
	struct wlr_primary_selection_source *source;
	const char *mime;
	int fds[2];

	g_return_val_if_fail(GOWL_IS_SEAT(self), NULL);

	seat = (struct wlr_seat *)self->wlr_seat;
	if (seat == NULL)
		return NULL;

	source = seat->primary_selection_source;
	if (source == NULL)
		return NULL;

	if (mime_array_contains(&source->mime_types,
	                        "text/plain;charset=utf-8"))
		mime = "text/plain;charset=utf-8";
	else if (mime_array_contains(&source->mime_types,
	                             "text/plain"))
		mime = "text/plain";
	else if (mime_array_contains(&source->mime_types,
	                             "UTF8_STRING"))
		mime = "UTF8_STRING";
	else
		return NULL;

	if (pipe(fds) != 0)
		return NULL;

	wlr_primary_selection_source_send(source, mime, fds[1]);

	return read_text_from_source_pipe(fds[0]);
}

/**
 * gowl_seat_set_primary_selection:
 * @self: a #GowlSeat
 * @text: the UTF-8 text to place in the primary selection
 *
 * Sets the primary selection to @text.
 */
void
gowl_seat_set_primary_selection(
	GowlSeat    *self,
	const gchar *text
){
	struct wlr_seat *seat;
	struct gowl_primary_text_source *ts;
	char *mime_plain, *mime_utf8, *mime_utf8_str;

	g_return_if_fail(GOWL_IS_SEAT(self));
	g_return_if_fail(text != NULL);

	seat = (struct wlr_seat *)self->wlr_seat;
	if (seat == NULL)
		return;

	ts = g_new0(struct gowl_primary_text_source, 1);
	ts->text = g_strdup(text);
	wlr_primary_selection_source_init(&ts->base,
	                                  &gowl_primary_text_source_impl);

	mime_plain = g_strdup("text/plain");
	mime_utf8  = g_strdup("text/plain;charset=utf-8");
	mime_utf8_str = g_strdup("UTF8_STRING");

	{
		char **slot;
		slot = wl_array_add(&ts->base.mime_types, sizeof(char *));
		*slot = mime_plain;
		slot = wl_array_add(&ts->base.mime_types, sizeof(char *));
		*slot = mime_utf8;
		slot = wl_array_add(&ts->base.mime_types, sizeof(char *));
		*slot = mime_utf8_str;
	}

	wlr_seat_set_primary_selection(seat, &ts->base,
	                               wl_display_next_serial(seat->display));
}

/* ------------------------------------------------------------------
 * Async clipboard/primary-selection reads
 *
 * Synchronous pipe reads deadlock when the current source is
 * client-owned and the handler runs inside the wl dispatch loop:
 * wlr_data_source_send() queues an event in the client's output
 * buffer which isn't flushed until the dispatch cycle returns, but
 * we'd be blocking in read() before that happens.  The async variant
 * registers an fd watch via wl_event_loop_add_fd() and returns
 * immediately, so the dispatch loop flushes events to the client,
 * the client writes to the pipe, and our POLLIN callback later
 * drains the data.
 * ------------------------------------------------------------------ */

typedef struct {
	int                           read_fd;
	GString                      *buf;
	struct wl_event_source       *source;
	GowlSeatClipboardReadCallback callback;
	gpointer                      user_data;
} GowlSeatAsyncRead;

static void
gowl_seat_async_read_finish(GowlSeatAsyncRead *ctx)
{
	gchar *text = NULL;

	if (ctx->source != NULL)
		wl_event_source_remove(ctx->source);
	if (ctx->read_fd >= 0)
		close(ctx->read_fd);

	if (ctx->buf != NULL && ctx->buf->len > 0)
		text = g_string_free(ctx->buf, FALSE);
	else if (ctx->buf != NULL)
		g_string_free(ctx->buf, TRUE);

	ctx->callback(text, ctx->user_data);
	g_free(ctx);
}

static int
gowl_seat_async_read_fd_cb(int fd, uint32_t mask, void *data)
{
	GowlSeatAsyncRead *ctx = (GowlSeatAsyncRead *)data;
	gchar tmp[4096];
	ssize_t n;

	(void)fd;

	if (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)) {
		/* Still drain whatever data is readable before closing. */
		while ((n = read(ctx->read_fd, tmp, sizeof(tmp))) > 0)
			g_string_append_len(ctx->buf, tmp, (gssize)n);
		gowl_seat_async_read_finish(ctx);
		return 0;
	}

	if (mask & WL_EVENT_READABLE) {
		for (;;) {
			n = read(ctx->read_fd, tmp, sizeof(tmp));
			if (n > 0) {
				g_string_append_len(ctx->buf, tmp, (gssize)n);
				continue;
			}
			if (n == 0) {
				/* EOF -- client finished writing. */
				gowl_seat_async_read_finish(ctx);
				return 0;
			}
			if (errno == EAGAIN || errno == EINTR) {
				/* More may come later; keep watching. */
				return 0;
			}
			/* Hard error */
			gowl_seat_async_read_finish(ctx);
			return 0;
		}
	}

	return 0;
}

/* Pick best MIME for plain-text from a wl_array of char* entries. */
static const char *
pick_text_mime(const struct wl_array *mimes)
{
	if (mime_array_contains(mimes, "text/plain;charset=utf-8"))
		return "text/plain;charset=utf-8";
	if (mime_array_contains(mimes, "text/plain"))
		return "text/plain";
	if (mime_array_contains(mimes, "UTF8_STRING"))
		return "UTF8_STRING";
	return NULL;
}

static gboolean
gowl_seat_start_async_read(struct wl_event_loop          *loop,
                           int                            read_fd,
                           GowlSeatClipboardReadCallback  cb,
                           gpointer                       user_data)
{
	GowlSeatAsyncRead *ctx;

	/* Make the read end non-blocking so our fd-watch callback can
	   drain-in-a-loop without risk of stalling the dispatch thread. */
	{
		int flags = fcntl(read_fd, F_GETFL, 0);
		if (flags >= 0)
			fcntl(read_fd, F_SETFL, flags | O_NONBLOCK);
		fcntl(read_fd, F_SETFD, FD_CLOEXEC);
	}

	ctx = g_new0(GowlSeatAsyncRead, 1);
	ctx->read_fd   = read_fd;
	ctx->buf       = g_string_new(NULL);
	ctx->callback  = cb;
	ctx->user_data = user_data;

	ctx->source = wl_event_loop_add_fd(loop, read_fd,
	                                   WL_EVENT_READABLE,
	                                   gowl_seat_async_read_fd_cb, ctx);
	if (ctx->source == NULL) {
		g_string_free(ctx->buf, TRUE);
		close(read_fd);
		g_free(ctx);
		return FALSE;
	}

	return TRUE;
}

gboolean
gowl_seat_read_clipboard_async(
	GowlSeat                      *self,
	struct wl_event_loop          *loop,
	GowlSeatClipboardReadCallback  callback,
	gpointer                       user_data)
{
	struct wlr_seat *seat;
	struct wlr_data_source *source;
	const char *mime;
	int fds[2];

	g_return_val_if_fail(GOWL_IS_SEAT(self), FALSE);
	g_return_val_if_fail(loop != NULL, FALSE);
	g_return_val_if_fail(callback != NULL, FALSE);

	seat = (struct wlr_seat *)self->wlr_seat;
	if (seat == NULL)
		return FALSE;

	source = seat->selection_source;
	if (source == NULL)
		return FALSE;

	mime = pick_text_mime(&source->mime_types);
	if (mime == NULL)
		return FALSE;

	if (pipe(fds) != 0)
		return FALSE;

	/* wlr's client-source .send impl closes fds[1] after queuing the
	   wl_data_source.send event; for compositor-owned sources the
	   impl writes and closes synchronously.  Either way we never
	   touch fds[1] again. */
	wlr_data_source_send(source, mime, fds[1]);

	if (!gowl_seat_start_async_read(loop, fds[0], callback, user_data)) {
		/* start_async_read already closed fds[0] on failure. */
		return FALSE;
	}

	return TRUE;
}

gboolean
gowl_seat_read_primary_selection_async(
	GowlSeat                      *self,
	struct wl_event_loop          *loop,
	GowlSeatClipboardReadCallback  callback,
	gpointer                       user_data)
{
	struct wlr_seat *seat;
	struct wlr_primary_selection_source *source;
	const char *mime;
	int fds[2];

	g_return_val_if_fail(GOWL_IS_SEAT(self), FALSE);
	g_return_val_if_fail(loop != NULL, FALSE);
	g_return_val_if_fail(callback != NULL, FALSE);

	seat = (struct wlr_seat *)self->wlr_seat;
	if (seat == NULL)
		return FALSE;

	source = seat->primary_selection_source;
	if (source == NULL)
		return FALSE;

	mime = pick_text_mime(&source->mime_types);
	if (mime == NULL)
		return FALSE;

	if (pipe(fds) != 0)
		return FALSE;

	wlr_primary_selection_source_send(source, mime, fds[1]);

	if (!gowl_seat_start_async_read(loop, fds[0], callback, user_data))
		return FALSE;

	return TRUE;
}

void
gowl_seat_emit_clipboard_changed(GowlSeat *self)
{
	g_return_if_fail(GOWL_IS_SEAT(self));
	g_signal_emit(self, seat_signals[SIGNAL_CLIPBOARD_CHANGED], 0);
}

void
gowl_seat_emit_primary_selection_changed(GowlSeat *self)
{
	g_return_if_fail(GOWL_IS_SEAT(self));
	g_signal_emit(self, seat_signals[SIGNAL_PRIMARY_SELECTION_CHANGED], 0);
}

/**
 * gowl_seat_push_focus_redirect:
 *
 * Saves the current focused client into a new #GowlFocusToken,
 * then transfers focus to @target.  The push/pop flow is
 * deliberately stack-free on the seat side — tokens are plain
 * boxed values owned by the caller, so nested redirects (two
 * simultaneous prefix sequences, for instance) are safe as long
 * as the consumer maintains matching token lifetimes.
 *
 * Emits `focus-redirected (from, to, reason)` after the focus
 * change takes effect.
 */
GowlFocusToken *
gowl_seat_push_focus_redirect(GowlSeat         *self,
                               gpointer          target,
                               GowlFocusReason   reason)
{
	gpointer        saved;
	GowlFocusToken *token;

	g_return_val_if_fail(GOWL_IS_SEAT(self), NULL);

	saved = self->focused_client;
	token = gowl_focus_token_new(saved, reason);

	gowl_seat_set_focused_client(self, target);

	g_signal_emit(self, seat_signals[SIGNAL_FOCUS_REDIRECTED], 0,
	              saved, target, reason);

	return token;
}

/**
 * gowl_seat_pop_focus_redirect:
 *
 * Restores the focused client from @token.  Frees the token.
 * Emits `focus-restored (reason)` afterwards.
 *
 * Calling with a %NULL token is a no-op (returns silently) so
 * callers that track "am I in a redirect?" with a single Option-
 * style pointer do not need separate guards.
 */
void
gowl_seat_pop_focus_redirect(GowlSeat        *self,
                              GowlFocusToken  *token)
{
	gpointer        saved;
	GowlFocusReason reason;

	g_return_if_fail(GOWL_IS_SEAT(self));

	if (token == NULL)
		return;

	saved  = gowl_focus_token_get_saved_client(token);
	reason = gowl_focus_token_get_reason(token);

	gowl_seat_set_focused_client(self, saved);

	g_signal_emit(self, seat_signals[SIGNAL_FOCUS_RESTORED], 0,
	              reason);

	gowl_focus_token_free(token);
}
