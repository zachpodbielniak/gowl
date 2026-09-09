/* Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later */

/*
 * clipboard -- a history of what has been on the clipboard.
 *
 * The compositor is the only thing that sees every selection change, so
 * this is where the history belongs: a client that copies and exits
 * still leaves its text behind, and nothing has to be running in the
 * background to notice.
 *
 * Reading a selection is the part that needs care.  The data lives in
 * the OWNING CLIENT, and getting it means asking that client to write
 * it down a pipe.  A synchronous read of that pipe on the compositor
 * thread is fine for a line of text and a frozen desktop for a
 * megabyte of PNG, because a client that writes slowly holds the
 * compositor for exactly as long as it likes and one that never writes
 * holds it forever.  So every read here is driven by the main loop and
 * bounded by a size cap and a deadline.
 *
 * Two surfaces read what this stores: the bar's `clipboard' widget and
 * cmacs.  They share a store rather than a protocol -- an index file
 * and one blob per entry -- because both of them want to show the same
 * list and either of them may delete from it.
 *
 * Reached by name through ipc_command:
 *
 *   clipboard-list            the index, newest first
 *   clipboard-copy <id>       put that entry back on the clipboard
 *   clipboard-delete <id>     forget one entry
 *   clipboard-clear           forget all of them
 *   clipboard-path <id>       where the blob is, for a reader
 */

#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "gowl-clipboard"
#include "core/gowl-core-private.h"
#include "core/gowl-seat.h"
#include "interfaces/gowl-startup-handler.h"
#include "interfaces/gowl-ipc-handler.h"

#include <glib-unix.h>
#include <glib/gstdio.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>

/* Images are the reason this is megabytes rather than kilobytes; a cap
   is still needed, because a clipboard can hold a whole video frame. */
#define CLIP_MAX_BYTES   (16 * 1024 * 1024)
#define CLIP_MAX_ENTRIES (100)
/* A client that stops writing must not hold a pipe open forever. */
#define CLIP_READ_TIMEOUT_MS (4000)

typedef struct {
	GowlModule parent;
	GowlCompositor *compositor;
	gchar   *dir;
	gulong   changed_id;
	guint64  next_id;
	/*
	 * Set while this module is the one putting something on the
	 * clipboard.  Without it, restoring an entry looks exactly like a
	 * new copy and the history grows a duplicate every time you paste
	 * from it.
	 */
	gboolean restoring;
} GowlClipboard;

typedef struct { GowlModuleClass parent; } GowlClipboardClass;

static void clip_startup_init(GowlStartupHandlerInterface *iface);
static void clip_ipc_init(GowlIpcHandlerInterface *iface);

G_DEFINE_TYPE_WITH_CODE(GowlClipboard, gowl_clipboard, GOWL_TYPE_MODULE,
	G_IMPLEMENT_INTERFACE(GOWL_TYPE_STARTUP_HANDLER, clip_startup_init)
	G_IMPLEMENT_INTERFACE(GOWL_TYPE_IPC_HANDLER, clip_ipc_init))

/* ── Store ───────────────────────────────────────────────────────── */

static const gchar *
clip_dir(GowlClipboard *self)
{
	if (self->dir == NULL) {
		self->dir = g_build_filename(g_get_user_state_dir(), "gowl",
		                             "clipboard", NULL);
		g_mkdir_with_parents(self->dir, 0700);
	}
	return self->dir;
}

static gchar *
clip_index_path(GowlClipboard *self)
{
	return g_build_filename(clip_dir(self), "index", NULL);
}

static gchar *
clip_blob_path(GowlClipboard *self, guint64 id)
{
	g_autofree gchar *name = g_strdup_printf("%" G_GUINT64_FORMAT ".bin",
	                                         id);

	return g_build_filename(clip_dir(self), name, NULL);
}

/* The index, newest first, as a list of lines.  Missing is empty. */
static GPtrArray *
clip_index_read(GowlClipboard *self)
{
	g_autofree gchar *path = clip_index_path(self);
	g_autofree gchar *body = NULL;
	GPtrArray *lines = g_ptr_array_new_with_free_func(g_free);

	if (g_file_get_contents(path, &body, NULL, NULL)) {
		g_auto(GStrv) split = g_strsplit(body, "\n", -1);
		gint i;

		for (i = 0; split[i] != NULL; i++) {
			if (*split[i] != '\0')
				g_ptr_array_add(lines, g_strdup(split[i]));
		}
	}
	return lines;
}

static void
clip_index_write(GowlClipboard *self, GPtrArray *lines)
{
	g_autofree gchar *path = clip_index_path(self);
	g_autoptr(GString) out = g_string_new(NULL);
	guint i;

	for (i = 0; i < lines->len; i++)
		g_string_append_printf(out, "%s\n",
		                       (const gchar *)g_ptr_array_index(lines, i));

	if (!g_file_set_contents(path, out->str, (gssize)out->len, NULL))
		g_warning("clipboard: cannot write %s", path);
}

/* The id a line names, or 0 -- ids start at 1 so 0 means "not one". */
static guint64
clip_line_id(const gchar *line)
{
	return g_ascii_strtoull(line, NULL, 10);
}

/*
 * A single-line, printable summary of the content.
 *
 * The index is read by a shell, by the bar and by Emacs, so a preview
 * with a newline or a tab in it would corrupt the row it lives on.
 */
static gchar *
clip_preview(const gchar *mime, const guint8 *data, gsize len)
{
	GString *out;
	gsize    i, taken = 0;

	if (mime != NULL && g_str_has_prefix(mime, "image/")) {
		return g_strdup_printf("[%s, %" G_GSIZE_FORMAT " bytes]",
		                       mime, len);
	}

	out = g_string_new(NULL);
	for (i = 0; i < len && taken < 120; i++) {
		guchar c = data[i];

		if (c == '\n' || c == '\t' || c == '\r') {
			/* One space for any run of whitespace: a preview is
			   a shape, not a transcription. */
			if (out->len > 0 && out->str[out->len - 1] != ' ')
				g_string_append_c(out, ' ');
			continue;
		}
		if (c < 0x20 || c == 0x7f)
			continue;
		g_string_append_c(out, (gchar)c);
		taken++;
	}
	if (out->len == 0)
		g_string_append_printf(out, "[%" G_GSIZE_FORMAT " bytes]", len);
	return g_string_free(out, FALSE);
}

static void
clip_store(GowlClipboard *self, const gchar *mime, const guint8 *data,
           gsize len)
{
	g_autoptr(GPtrArray) lines = NULL;
	g_autofree gchar *preview = NULL;
	g_autofree gchar *blob = NULL;
	g_autofree gchar *line = NULL;
	guint64 id;

	if (len == 0)
		return;

	lines = clip_index_read(self);

	/*
	 * Skip an exact repeat of the newest entry.  Selections are
	 * re-announced for reasons that have nothing to do with the user
	 * copying again -- a focus change is enough -- and a history full
	 * of the same line is not a history.
	 */
	if (lines->len > 0) {
		g_autofree gchar *newest_blob = NULL;
		g_autofree gchar *newest = NULL;
		gsize newest_len = 0;

		newest_blob = clip_blob_path(self,
			clip_line_id(g_ptr_array_index(lines, 0)));
		if (g_file_get_contents(newest_blob, &newest, &newest_len,
		                        NULL)
		    && newest_len == len
		    && memcmp(newest, data, len) == 0)
			return;
	}

	id = self->next_id++;
	blob = clip_blob_path(self, id);
	if (!g_file_set_contents(blob, (const gchar *)data, (gssize)len,
	                         NULL)) {
		g_warning("clipboard: cannot write %s", blob);
		return;
	}
	g_chmod(blob, 0600);

	preview = clip_preview(mime, data, len);
	line = g_strdup_printf("%" G_GUINT64_FORMAT "\t%s\t%" G_GSIZE_FORMAT
	                       "\t%s", id, mime != NULL ? mime : "", len,
	                       preview);
	g_ptr_array_insert(lines, 0, g_steal_pointer(&line));

	/* Trim from the far end, deleting the blob with the row: an index
	   entry with no blob is a row that cannot be pasted. */
	while (lines->len > CLIP_MAX_ENTRIES) {
		const gchar *last = g_ptr_array_index(lines, lines->len - 1);
		g_autofree gchar *old = clip_blob_path(self,
		                                       clip_line_id(last));

		g_unlink(old);
		g_ptr_array_remove_index(lines, lines->len - 1);
	}

	clip_index_write(self, lines);
}

/* ── Reading the selection, without blocking ─────────────────────── */

typedef struct {
	GowlClipboard *module;
	gchar         *mime;
	GByteArray    *buf;
	gint           fd;
	guint          timeout_id;
} ClipRead;

static void
clip_read_free(gpointer data)
{
	ClipRead *r = data;

	if (r->timeout_id != 0)
		g_source_remove(r->timeout_id);
	if (r->fd >= 0)
		close(r->fd);
	g_byte_array_unref(r->buf);
	g_free(r->mime);
	g_free(r);
}

static gboolean
clip_read_timeout(gpointer data)
{
	ClipRead *r = data;

	/* The client stopped writing.  Whatever arrived is not worth
	   keeping -- a truncated clipboard entry is worse than none. */
	g_debug("clipboard: read of %s timed out", r->mime);
	r->timeout_id = 0;
	close(r->fd);
	r->fd = -1;
	return G_SOURCE_REMOVE;
}

static gboolean
clip_read_cb(gint fd, GIOCondition condition, gpointer data)
{
	ClipRead *r = data;
	guint8    chunk[16384];

	for (;;) {
		ssize_t n = read(fd, chunk, sizeof(chunk));

		if (n > 0) {
			if (r->buf->len + (guint)n > CLIP_MAX_BYTES) {
				g_debug("clipboard: %s exceeds the cap",
				        r->mime);
				return G_SOURCE_REMOVE;
			}
			g_byte_array_append(r->buf, chunk, (guint)n);
			continue;
		}
		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
			return G_SOURCE_CONTINUE;
		break;                          /* EOF, or the pipe broke */
	}

	if ((condition & G_IO_ERR) == 0 && r->buf->len > 0)
		clip_store(r->module, r->mime, r->buf->data, r->buf->len);
	return G_SOURCE_REMOVE;
}

/* The MIME type worth keeping, in order of preference. */
static const gchar *
clip_pick_mime(struct wlr_data_source *source)
{
	static const gchar *const wanted[] = {
		"text/plain;charset=utf-8", "text/plain", "UTF8_STRING",
		"image/png", "image/jpeg", "image/bmp",
	};
	gsize i;
	char **p;

	for (i = 0; i < G_N_ELEMENTS(wanted); i++) {
		wl_array_for_each(p, &source->mime_types) {
			if (g_strcmp0(*p, wanted[i]) == 0)
				return wanted[i];
		}
	}
	return NULL;
}

static void
clip_on_clipboard_changed(GowlSeat *seat, gpointer data)
{
	GowlClipboard *self = data;
	struct wlr_seat *wlr_seat;
	struct wlr_data_source *source;
	const gchar *mime;
	ClipRead *r;
	gint fds[2];

	if (self->restoring || self->compositor == NULL)
		return;

	/* GowlSeat keeps the wlr_seat as an alias field; the private
	   header is what a module gets it through. */
	wlr_seat = (struct wlr_seat *)seat->wlr_seat;
	if (wlr_seat == NULL)
		return;
	source = wlr_seat->selection_source;
	if (source == NULL)
		return;

	mime = clip_pick_mime(source);
	if (mime == NULL)
		return;                 /* nothing here we know how to keep */

	if (!g_unix_open_pipe(fds, FD_CLOEXEC, NULL))
		return;

	/* The client writes into fds[1]; we drain fds[0] from the main
	   loop.  Non-blocking, because the first read of a large payload
	   would otherwise wait on a client that has not written yet. */
	if (!g_unix_set_fd_nonblocking(fds[0], TRUE, NULL)) {
		close(fds[0]);
		close(fds[1]);
		return;
	}

	wlr_data_source_send(source, mime, fds[1]);
	close(fds[1]);

	r = g_new0(ClipRead, 1);
	r->module = self;
	r->mime = g_strdup(mime);
	r->buf = g_byte_array_new();
	r->fd = fds[0];
	r->timeout_id = g_timeout_add(CLIP_READ_TIMEOUT_MS,
	                              clip_read_timeout, r);
	g_unix_fd_add_full(G_PRIORITY_DEFAULT, fds[0],
	                   G_IO_IN | G_IO_ERR | G_IO_HUP,
	                   clip_read_cb, r, clip_read_free);
}

/* ── Commands ────────────────────────────────────────────────────── */

static gchar *
clip_handle_command(GowlIpcHandler *handler, const gchar *command,
                    const gchar *args)
{
	GowlClipboard *self = (GowlClipboard *)handler;
	g_autoptr(GPtrArray) lines = NULL;
	guint64 id;
	guint i;

	if (command == NULL || self->compositor == NULL)
		return NULL;

	if (g_strcmp0(command, "clipboard-list") == 0) {
		g_autoptr(GString) out = g_string_new(NULL);

		lines = clip_index_read(self);
		for (i = 0; i < lines->len; i++)
			g_string_append_printf(out, "%s\n",
				(const gchar *)g_ptr_array_index(lines, i));
		return g_string_free(g_steal_pointer(&out), FALSE);
	}

	if (g_strcmp0(command, "clipboard-clear") == 0) {
		lines = clip_index_read(self);
		for (i = 0; i < lines->len; i++) {
			g_autofree gchar *blob = clip_blob_path(self,
				clip_line_id(g_ptr_array_index(lines, i)));

			g_unlink(blob);
		}
		g_ptr_array_set_size(lines, 0);
		clip_index_write(self, lines);
		return g_strdup("OK cleared");
	}

	if (args == NULL || *args == '\0')
		return NULL;
	id = g_ascii_strtoull(args, NULL, 10);
	if (id == 0)
		return g_strdup("ERROR a numeric id is required");

	if (g_strcmp0(command, "clipboard-path") == 0) {
		g_autofree gchar *blob = clip_blob_path(self, id);

		if (!g_file_test(blob, G_FILE_TEST_EXISTS))
			return g_strdup("ERROR no such entry");
		return g_strdup_printf("%s\n", blob);
	}

	if (g_strcmp0(command, "clipboard-delete") == 0) {
		g_autofree gchar *blob = clip_blob_path(self, id);
		gboolean found = FALSE;

		lines = clip_index_read(self);
		for (i = 0; i < lines->len; i++) {
			if (clip_line_id(g_ptr_array_index(lines, i)) == id) {
				g_ptr_array_remove_index(lines, i);
				found = TRUE;
				break;
			}
		}
		if (!found)
			return g_strdup("ERROR no such entry");
		g_unlink(blob);
		clip_index_write(self, lines);
		return g_strdup("OK deleted");
	}

	if (g_strcmp0(command, "clipboard-copy") == 0) {
		g_autofree gchar *blob = clip_blob_path(self, id);
		g_autofree gchar *body = NULL;
		g_autoptr(GBytes) bytes = NULL;
		g_autofree gchar *mime = NULL;
		gsize len = 0;
		GowlSeat *seat;

		lines = clip_index_read(self);
		for (i = 0; i < lines->len; i++) {
			const gchar *l = g_ptr_array_index(lines, i);

			if (clip_line_id(l) == id) {
				g_auto(GStrv) f = g_strsplit(l, "\t", 4);

				if (f[1] != NULL)
					mime = g_strdup(f[1]);
				break;
			}
		}
		if (!g_file_get_contents(blob, &body, &len, NULL))
			return g_strdup("ERROR no such entry");

		seat = gowl_compositor_get_seat(self->compositor);
		if (seat == NULL)
			return g_strdup("ERROR no seat");

		bytes = g_bytes_new_take(g_steal_pointer(&body), len);
		/* Guarded: putting it back is a selection change like any
		   other, and without this the history grows a duplicate
		   every time something is pasted from it. */
		self->restoring = TRUE;
		gowl_seat_set_clipboard_bytes(seat, bytes,
			(mime != NULL && *mime != '\0') ? mime : "text/plain");
		self->restoring = FALSE;
		return g_strdup("OK copied");
	}

	return NULL;
}

/* ── Lifecycle ───────────────────────────────────────────────────── */

static void
clip_detach(GowlClipboard *self)
{
	if (self->compositor != NULL) {
		GowlSeat *seat = gowl_compositor_get_seat(self->compositor);

		if (seat != NULL && self->changed_id != 0)
			g_signal_handler_disconnect(seat, self->changed_id);
		g_object_remove_weak_pointer(G_OBJECT(self->compositor),
		                             (gpointer *)&self->compositor);
		self->compositor = NULL;
	}
	self->changed_id = 0;
}

static void
clip_startup(GowlStartupHandler *handler, gpointer compositor)
{
	GowlClipboard *self = (GowlClipboard *)handler;
	g_autoptr(GPtrArray) lines = NULL;
	GowlSeat *seat;
	guint i;

	if (self->compositor == compositor)
		return;
	clip_detach(self);

	self->compositor = compositor;
	g_object_add_weak_pointer(G_OBJECT(compositor),
	                          (gpointer *)&self->compositor);

	/* Ids continue from what is on disk, so a restart does not reuse
	   an id whose blob is still there. */
	lines = clip_index_read(self);
	self->next_id = 1;
	for (i = 0; i < lines->len; i++) {
		guint64 id = clip_line_id(g_ptr_array_index(lines, i));

		if (id >= self->next_id)
			self->next_id = id + 1;
	}

	seat = gowl_compositor_get_seat(compositor);
	if (seat != NULL)
		self->changed_id = g_signal_connect(seat, "clipboard-changed",
			G_CALLBACK(clip_on_clipboard_changed), self);
}

static void clip_startup_init(GowlStartupHandlerInterface *iface)
{
	iface->on_startup = clip_startup;
}

static void clip_ipc_init(GowlIpcHandlerInterface *iface)
{
	iface->handle_command = clip_handle_command;
}

static gboolean clip_activate(GowlModule *m) { (void)m; return TRUE; }
static void clip_deactivate(GowlModule *m) { clip_detach((GowlClipboard *)m); }
static const gchar *clip_name(GowlModule *m) { (void)m; return "clipboard"; }

static void
clip_finalize(GObject *object)
{
	GowlClipboard *self = (GowlClipboard *)object;

	clip_detach(self);
	g_free(self->dir);
	G_OBJECT_CLASS(gowl_clipboard_parent_class)->finalize(object);
}

static void
gowl_clipboard_class_init(GowlClipboardClass *klass)
{
	GowlModuleClass *module = GOWL_MODULE_CLASS(klass);

	module->activate = clip_activate;
	module->deactivate = clip_deactivate;
	module->get_name = clip_name;
	G_OBJECT_CLASS(klass)->finalize = clip_finalize;
}

static void
gowl_clipboard_init(GowlClipboard *self)
{
	self->next_id = 1;
}

G_MODULE_EXPORT GType gowl_module_register(void)
{
	return gowl_clipboard_get_type();
}
