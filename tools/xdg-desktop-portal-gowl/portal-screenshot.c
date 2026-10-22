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
 * portal-screenshot.c - org.freedesktop.impl.portal.Screenshot.
 *
 * Screenshot() writes a PNG and answers with its file:// URI; the
 * frontend takes it from there (a sandboxed application gets it
 * through the document portal).  PickColor() reads one pixel.  Both
 * capture through portal-capture.c, the same ext-image-copy-capture
 * session a screen cast opens, for one frame.
 *
 * WHAT IS SHOT.  The v3 `target' option, when given:
 *
 *   1 Screen         the focused output; with `interactive' and more
 *                    than one output, the chooser picks
 *   2 Window         a window, from the chooser
 *   4 Area           a rectangle, from slurp
 *                    ($GOWL_PORTAL_AREA_PICKER overrides it)
 *   8 Active Window  the focused window, as gowl's IPC reports it
 *
 * With no target, a non-interactive call shoots the focused output
 * (what every Screenshot backend did before v3) and an interactive one
 * offers everything in one chooser: the outputs, the windows, and an
 * "Area" row when there is a picker.  AvailableTargets is computed at
 * the moment it is read, so it never advertises Area without a picker
 * or Active Window outside gowl.
 *
 * WHERE THE PIXEL IS.  PickColor's point comes from
 * $GOWL_PORTAL_POINT_PICKER, else `slurp -p', else -- inside gowl --
 * the pointer's current position.  Layout coordinates become a pixel
 * of the output under them through wl_output's position, mode and
 * integer scale; a rotated or fractionally scaled output is located
 * correctly but sampled at the rounded pixel.
 *
 * WHERE THE FILE GOES.  $GOWL_PORTAL_SCREENSHOT_DIR, else
 * $XDG_PICTURES_DIR/Screenshots, named the way GNOME names them.
 */

#include "portal-screenshot.h"
#include "portal-chooser.h"
#include "portal-gowl.h"

#include <cairo.h>
#include <glib/gstdio.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#define PORTAL_RESPONSE_SUCCESS   0
#define PORTAL_RESPONSE_CANCELLED 1
#define PORTAL_RESPONSE_OTHER     2

/* Screenshot v3 targets. */
#define TARGET_SCREEN        1
#define TARGET_WINDOW        2
#define TARGET_AREA          4
#define TARGET_ACTIVE_WINDOW 8

#define SCREENSHOT_VERSION 3
#define SCREENSHOT_PATH    "/org/freedesktop/portal/desktop"

/* One frame should take a frame or two; this is the compositor being
 * wedged, not slow. */
#define GRAB_TIMEOUT_MS (3000)

struct _PortalScreenshot {
	GDBusConnection *conn;
	guint            reg_id;
	PortalCapture   *capture;   /* borrowed */
	gboolean         can_window;
};

static GDBusNodeInfo *ss_node_info;

static const gchar ss_introspection_xml[] =
"<node>"
"  <interface name='org.freedesktop.impl.portal.Screenshot'>"
"    <property name='version' type='u' access='read'/>"
"    <property name='AvailableTargets' type='u' access='read'/>"
"    <method name='Screenshot'>"
"      <arg type='o' name='handle' direction='in'/>"
"      <arg type='s' name='app_id' direction='in'/>"
"      <arg type='s' name='parent_window' direction='in'/>"
"      <arg type='a{sv}' name='options' direction='in'/>"
"      <arg type='u' name='response' direction='out'/>"
"      <arg type='a{sv}' name='results' direction='out'/>"
"    </method>"
"    <method name='PickColor'>"
"      <arg type='o' name='handle' direction='in'/>"
"      <arg type='s' name='app_id' direction='in'/>"
"      <arg type='s' name='parent_window' direction='in'/>"
"      <arg type='a{sv}' name='options' direction='in'/>"
"      <arg type='u' name='response' direction='out'/>"
"      <arg type='a{sv}' name='results' direction='out'/>"
"    </method>"
"  </interface>"
"</node>";

/* A rectangle in layout (logical) coordinates. */
typedef struct {
	gint32 x, y, w, h;
} Rect;

/* ---------------------------------------------------------------
 * Asking gowl
 * --------------------------------------------------------------- */

/* A string member of a wire-protocol call's object result, or NULL. */
static gchar *
gowl_string(const gchar *method, const gchar *member)
{
	g_autoptr(PortalGowl) gowl = portal_gowl_connect(NULL);
	g_autoptr(JsonNode) result = NULL;

	if (gowl == NULL)
		return NULL;
	result = portal_gowl_call(gowl, method, NULL, NULL);
	if (result == NULL || !JSON_NODE_HOLDS_OBJECT(result))
		return NULL;
	return g_strdup(json_object_get_string_member_with_default(
		json_node_get_object(result), member, NULL));
}

static gboolean
gowl_reachable(void)
{
	g_autoptr(PortalGowl) gowl = portal_gowl_connect(NULL);
	g_autoptr(JsonNode) pong = NULL;

	if (gowl == NULL)
		return FALSE;
	pong = portal_gowl_call(gowl, "rpc.ping", NULL, NULL);
	return pong != NULL;
}

/* The focused window's application and title, inside gowl. */
static gboolean
gowl_focused_window(gchar **app_id, gchar **title)
{
	g_autoptr(PortalGowl) gowl = portal_gowl_connect(NULL);
	g_autoptr(JsonNode) result = NULL;
	JsonObject *w;

	if (gowl == NULL)
		return FALSE;
	result = portal_gowl_call(gowl, "window.focused", NULL, NULL);
	if (result == NULL || !JSON_NODE_HOLDS_OBJECT(result))
		return FALSE;
	w = json_node_get_object(result);
	*app_id = g_strdup(json_object_get_string_member_with_default(w,
		"app_id", ""));
	*title = g_strdup(json_object_get_string_member_with_default(w,
		"title", ""));
	return TRUE;
}

/* ---------------------------------------------------------------
 * Pickers
 * --------------------------------------------------------------- */

/* The area picker command, or NULL when there is none. */
static gchar *
area_picker(void)
{
	const gchar *env = g_getenv("GOWL_PORTAL_AREA_PICKER");
	g_autofree gchar *slurp = NULL;

	if (env != NULL && *env != '\0')
		return g_strdup(env);
	slurp = g_find_program_in_path("slurp");
	return slurp != NULL ? g_strdup_printf("%s -f '%%x,%%y %%wx%%h'", slurp)
	                     : NULL;
}

/* The point picker command, or NULL when there is none. */
static gchar *
point_picker(void)
{
	const gchar *env = g_getenv("GOWL_PORTAL_POINT_PICKER");
	g_autofree gchar *slurp = NULL;

	if (env != NULL && *env != '\0')
		return g_strdup(env);
	slurp = g_find_program_in_path("slurp");
	return slurp != NULL ? g_strdup_printf("%s -p -f '%%x,%%y 1x1'", slurp)
	                     : NULL;
}

/*
 * Runs a picker and reads `X,Y WxH' from it (slurp's format; a bare
 * `X,Y' is a 1x1 area).  An empty answer, or a picker that exits
 * non-zero, is the human backing out.
 */
static PortalChooserResult
run_picker(const gchar *cmd, Rect *out)
{
	g_auto(GStrv) argv = NULL;
	g_autoptr(GSubprocess) proc = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *text = NULL;
	gint n;

	if (!g_shell_parse_argv(cmd, NULL, &argv, &error)) {
		g_warning("screenshot: picker '%s' is unparseable: %s", cmd,
		          error->message);
		return PORTAL_CHOOSER_UNAVAILABLE;
	}
	proc = g_subprocess_newv((const gchar * const *)argv,
	                         G_SUBPROCESS_FLAGS_STDOUT_PIPE, &error);
	if (proc == NULL
	    || !g_subprocess_communicate_utf8(proc, NULL, NULL, &text, NULL,
	                                      &error)) {
		g_warning("screenshot: picker '%s' failed: %s", cmd,
		          error->message);
		return PORTAL_CHOOSER_UNAVAILABLE;
	}
	if (!g_subprocess_get_successful(proc) || text == NULL
	    || *g_strstrip(text) == '\0')
		return PORTAL_CHOOSER_CANCELLED;
	out->w = 1;
	out->h = 1;
	n = sscanf(text, "%d,%d %dx%d", &out->x, &out->y, &out->w, &out->h);
	if (n != 2 && n != 4) {
		g_warning("screenshot: picker said '%s', not `X,Y WxH'", text);
		return PORTAL_CHOOSER_CANCELLED;
	}
	if (out->w <= 0 || out->h <= 0)
		return PORTAL_CHOOSER_CANCELLED;
	return PORTAL_CHOOSER_CHOSEN;
}

/* Where to sample: a picker, else (inside gowl) the pointer. */
static PortalChooserResult
pick_point(Rect *out)
{
	g_autofree gchar *cmd = point_picker();
	g_autoptr(PortalGowl) gowl = NULL;
	g_autoptr(JsonNode) result = NULL;
	JsonObject *p;

	if (cmd != NULL)
		return run_picker(cmd, out);
	gowl = portal_gowl_connect(NULL);
	if (gowl == NULL)
		return PORTAL_CHOOSER_UNAVAILABLE;
	result = portal_gowl_call(gowl, "pointer.get", NULL, NULL);
	if (result == NULL || !JSON_NODE_HOLDS_OBJECT(result))
		return PORTAL_CHOOSER_UNAVAILABLE;
	p = json_node_get_object(result);
	out->x = (gint32)json_object_get_double_member_with_default(p, "x", 0);
	out->y = (gint32)json_object_get_double_member_with_default(p, "y", 0);
	out->w = 1;
	out->h = 1;
	return PORTAL_CHOOSER_CHOSEN;
}

/* ---------------------------------------------------------------
 * Sources
 * --------------------------------------------------------------- */

/* The output whose layout rectangle holds (x, y). */
static const PortalCaptureSource *
output_at(GPtrArray *sources, gint32 x, gint32 y)
{
	guint i;

	for (i = 0; i < sources->len; i++) {
		const PortalCaptureSource *s = g_ptr_array_index(sources, i);

		if (s->is_window)
			continue;
		if (x >= s->x && y >= s->y && x < s->x + s->width
		    && y < s->y + s->height)
			return s;
	}
	return NULL;
}

/* The focused output: gowl's answer when there is one, else the first. */
static const PortalCaptureSource *
focused_output(GPtrArray *sources)
{
	g_autofree gchar *name = gowl_string("monitor.get", "name");
	const PortalCaptureSource *first = NULL;
	guint i;

	for (i = 0; i < sources->len; i++) {
		const PortalCaptureSource *s = g_ptr_array_index(sources, i);

		if (s->is_window)
			continue;
		if (first == NULL)
			first = s;
		if (name != NULL && g_strcmp0(s->output_name, name) == 0)
			return s;
	}
	return first;
}

/* The active window, matched by application and title. */
static const PortalCaptureSource *
active_window(GPtrArray *sources)
{
	g_autofree gchar *app_id = NULL;
	g_autofree gchar *title = NULL;
	guint i;

	if (!gowl_focused_window(&app_id, &title))
		return NULL;
	for (i = 0; i < sources->len; i++) {
		const PortalCaptureSource *s = g_ptr_array_index(sources, i);

		if (s->is_window && g_strcmp0(s->app_id, app_id) == 0
		    && g_strcmp0(s->window_title, title) == 0)
			return s;
	}
	return NULL;
}

/*
 * The chooser over the sources @want accepts, plus an Area row when
 * @offer_area.  @area_chosen says the human took that row.
 */
static PortalChooserResult
choose(GPtrArray *sources, gboolean want_outputs, gboolean want_windows,
       gboolean offer_area, const PortalCaptureSource **chosen,
       gboolean *area_chosen)
{
	g_autoptr(GPtrArray) lines = g_ptr_array_new();
	g_autoptr(GPtrArray) map = g_ptr_array_new();
	PortalChooserResult result;
	guint index = 0;
	guint i;

	*chosen = NULL;
	*area_chosen = FALSE;
	for (i = 0; i < sources->len; i++) {
		const PortalCaptureSource *s = g_ptr_array_index(sources, i);

		if (s->is_window ? !want_windows : !want_outputs)
			continue;
		g_ptr_array_add(lines, s->title);
		g_ptr_array_add(map, (gpointer)s);
	}
	if (offer_area) {
		g_ptr_array_add(lines, (gpointer)"Area: select a region");
		g_ptr_array_add(map, NULL);
	}
	if (lines->len == 0)
		return PORTAL_CHOOSER_UNAVAILABLE;
	g_ptr_array_add(lines, NULL);

	result = portal_chooser_run("Screenshot",
	                            (const gchar * const *)lines->pdata, &index);
	if (result != PORTAL_CHOOSER_CHOSEN)
		return result;
	*chosen = g_ptr_array_index(map, index);
	*area_chosen = *chosen == NULL;
	return result;
}

/* ---------------------------------------------------------------
 * The file
 * --------------------------------------------------------------- */

static gchar *
screenshot_dir(void)
{
	const gchar *env = g_getenv("GOWL_PORTAL_SCREENSHOT_DIR");
	const gchar *pictures;

	if (env != NULL && *env != '\0')
		return g_strdup(env);
	pictures = g_get_user_special_dir(G_USER_DIRECTORY_PICTURES);
	if (pictures == NULL)
		pictures = g_get_home_dir();
	return g_build_filename(pictures, "Screenshots", NULL);
}

/* A fresh path: GNOME's naming, with a counter on a same-second clash. */
static gchar *
screenshot_path(GError **error)
{
	g_autofree gchar *dir = screenshot_dir();
	g_autoptr(GDateTime) now = g_date_time_new_now_local();
	g_autofree gchar *stamp = g_date_time_format(now, "%Y-%m-%d %H-%M-%S");
	guint i;

	if (g_mkdir_with_parents(dir, 0755) != 0) {
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
		            "cannot create %s: %s", dir, g_strerror(errno));
		return NULL;
	}
	for (i = 0; i < 1000; i++) {
		g_autofree gchar *name = i == 0
			? g_strdup_printf("Screenshot from %s.png", stamp)
			: g_strdup_printf("Screenshot from %s-%u.png", stamp, i);
		gchar *path = g_build_filename(dir, name, NULL);

		if (!g_file_test(path, G_FILE_TEST_EXISTS))
			return path;
		g_free(path);
	}
	g_set_error(error, G_IO_ERROR, G_IO_ERROR_EXISTS,
	            "too many screenshots named for %s", stamp);
	return NULL;
}

/*
 * @image, or the @crop of it (in its own pixels), as a PNG.
 *
 * cairo's ARGB32/RGB24 are a native-endian 32-bit word with alpha (or
 * nothing) on top -- exactly wl_shm's ARGB8888/XRGB8888 -- so those are
 * copied row by row; the ABGR orders are swapped on the way.
 */
static gboolean
write_png(const PortalCaptureImage *image, const Rect *crop,
          const gchar *path, GError **error)
{
	cairo_surface_t *surface;
	cairo_status_t status;
	cairo_format_t format;
	guint8 *dst;
	gint dst_stride;
	Rect r;
	gint row;
	gint col;

	r.x = 0;
	r.y = 0;
	r.w = (gint32)image->width;
	r.h = (gint32)image->height;
	if (crop != NULL)
		r = *crop;

	format = image->format == 0u || image->format == 0x34325241u
	         || image->format == 0x34324241u
	         ? CAIRO_FORMAT_ARGB32 : CAIRO_FORMAT_RGB24;
	surface = cairo_image_surface_create(format, r.w, r.h);
	if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NO_SPACE,
		                    "cannot allocate the image");
		cairo_surface_destroy(surface);
		return FALSE;
	}
	cairo_surface_flush(surface);
	dst = cairo_image_surface_get_data(surface);
	dst_stride = cairo_image_surface_get_stride(surface);
	for (row = 0; row < r.h; row++) {
		const guint8 *src = image->data
			+ (gsize)(r.y + row) * image->stride + (gsize)r.x * 4u;
		guint8 *out = dst + (gsize)row * (gsize)dst_stride;

		if (format == CAIRO_FORMAT_ARGB32 && image->format == 0x34324241u) {
			for (col = 0; col < r.w; col++) {
				out[col * 4 + 0] = src[col * 4 + 2];
				out[col * 4 + 1] = src[col * 4 + 1];
				out[col * 4 + 2] = src[col * 4 + 0];
				out[col * 4 + 3] = src[col * 4 + 3];
			}
		} else if (image->format == 0x34324258u) {
			for (col = 0; col < r.w; col++) {
				out[col * 4 + 0] = src[col * 4 + 2];
				out[col * 4 + 1] = src[col * 4 + 1];
				out[col * 4 + 2] = src[col * 4 + 0];
				out[col * 4 + 3] = 0xff;
			}
		} else {
			memcpy(out, src, (gsize)r.w * 4u);
		}
	}
	cairo_surface_mark_dirty(surface);
	status = cairo_surface_write_to_png(surface, path);
	cairo_surface_destroy(surface);
	if (status != CAIRO_STATUS_SUCCESS) {
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
		            "cannot write %s: %s", path,
		            cairo_status_to_string(status));
		return FALSE;
	}
	return TRUE;
}

/*
 * @area (layout coordinates) as pixels of @output's frame, clipped to
 * it.  An area reaching onto a second output is cut at the first one's
 * edge: one capture is one output.
 */
static gboolean
area_to_pixels(const PortalCaptureSource *output, const Rect *area,
               const PortalCaptureImage *image, Rect *out)
{
	gint32 scale = output->scale > 0 ? output->scale : 1;
	gint32 x0 = (area->x - output->x) * scale;
	gint32 y0 = (area->y - output->y) * scale;
	gint32 x1 = x0 + area->w * scale;
	gint32 y1 = y0 + area->h * scale;

	x0 = CLAMP(x0, 0, (gint32)image->width);
	y0 = CLAMP(y0, 0, (gint32)image->height);
	x1 = CLAMP(x1, 0, (gint32)image->width);
	y1 = CLAMP(y1, 0, (gint32)image->height);
	if (x1 <= x0 || y1 <= y0)
		return FALSE;
	out->x = x0;
	out->y = y0;
	out->w = x1 - x0;
	out->h = y1 - y0;
	return TRUE;
}

/* ---------------------------------------------------------------
 * The methods
 * --------------------------------------------------------------- */

static void
reply(GDBusMethodInvocation *invocation, guint32 response,
      GVariantBuilder *results)
{
	GVariantBuilder empty;

	if (results == NULL) {
		g_variant_builder_init(&empty, G_VARIANT_TYPE_VARDICT);
		results = &empty;
	}
	g_dbus_method_invocation_return_value(invocation,
		g_variant_new("(ua{sv})", response, results));
}

static guint32
available_targets(PortalScreenshot *self)
{
	g_autofree gchar *picker = area_picker();
	guint32 targets = TARGET_SCREEN;

	if (self->can_window)
		targets |= TARGET_WINDOW;
	if (picker != NULL)
		targets |= TARGET_AREA;
	if (self->can_window && gowl_reachable())
		targets |= TARGET_ACTIVE_WINDOW;
	return targets;
}

static void
handle_screenshot(PortalScreenshot *self, GVariant *params,
                  GDBusMethodInvocation *invocation)
{
	const gchar *handle;
	const gchar *app_id;
	const gchar *parent_window;
	g_autoptr(GVariant) options = NULL;
	g_autoptr(GPtrArray) sources = NULL;
	g_autoptr(PortalCaptureImage) image = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *path = NULL;
	g_autofree gchar *uri = NULL;
	g_autofree gchar *picker = NULL;
	const PortalCaptureSource *source = NULL;
	gboolean interactive = FALSE;
	gboolean use_area = FALSE;
	guint32 target = 0;
	Rect area;
	Rect crop;
	gboolean have_crop = FALSE;
	PortalChooserResult result = PORTAL_CHOOSER_CHOSEN;
	GVariantBuilder results;

	g_variant_get(params, "(&o&s&s@a{sv})", &handle, &app_id,
	              &parent_window, &options);
	g_variant_lookup(options, "interactive", "b", &interactive);
	g_variant_lookup(options, "target", "u", &target);
	memset(&area, 0, sizeof area);

	if (target != 0 && ((target & (target - 1)) != 0
	                    || (target & available_targets(self)) == 0)) {
		g_message("screenshot: '%s' asked for target %u, which is not "
		          "offered", app_id, target);
		reply(invocation, PORTAL_RESPONSE_OTHER, NULL);
		return;
	}

	sources = portal_capture_list_sources(self->capture, self->can_window,
	                                      TRUE);
	picker = area_picker();

	/* Decide what to shoot. */
	switch (target) {
	case TARGET_SCREEN:
		if (interactive)
			result = choose(sources, TRUE, FALSE, FALSE, &source,
			                &use_area);
		if (source == NULL && result != PORTAL_CHOOSER_CANCELLED) {
			source = focused_output(sources);
			result = PORTAL_CHOOSER_CHOSEN;
		}
		break;
	case TARGET_WINDOW:
		result = choose(sources, FALSE, TRUE, FALSE, &source, &use_area);
		break;
	case TARGET_AREA:
		use_area = TRUE;
		break;
	case TARGET_ACTIVE_WINDOW:
		source = active_window(sources);
		if (source == NULL)
			result = PORTAL_CHOOSER_UNAVAILABLE;
		break;
	default:
		if (!interactive) {
			source = focused_output(sources);
			break;
		}
		result = choose(sources, TRUE, self->can_window, picker != NULL,
		                &source, &use_area);
		/* No chooser at all: the screen, rather than nothing. */
		if (result == PORTAL_CHOOSER_UNAVAILABLE) {
			source = focused_output(sources);
			result = PORTAL_CHOOSER_CHOSEN;
		}
		break;
	}

	if (use_area && result == PORTAL_CHOOSER_CHOSEN) {
		result = picker != NULL ? run_picker(picker, &area)
		                        : PORTAL_CHOOSER_UNAVAILABLE;
		if (result == PORTAL_CHOOSER_CHOSEN) {
			source = output_at(sources, area.x, area.y);
			if (source == NULL)
				result = PORTAL_CHOOSER_UNAVAILABLE;
		}
	}
	if (result == PORTAL_CHOOSER_CANCELLED) {
		reply(invocation, PORTAL_RESPONSE_CANCELLED, NULL);
		return;
	}
	if (result != PORTAL_CHOOSER_CHOSEN || source == NULL) {
		g_message("screenshot: nothing to shoot for '%s'", app_id);
		reply(invocation, PORTAL_RESPONSE_OTHER, NULL);
		return;
	}

	image = portal_capture_grab(self->capture, source, FALSE,
	                            GRAB_TIMEOUT_MS, &error);
	if (image == NULL) {
		g_warning("screenshot: could not capture %s: %s", source->title,
		          error->message);
		reply(invocation, PORTAL_RESPONSE_OTHER, NULL);
		return;
	}
	if (use_area) {
		have_crop = area_to_pixels(source, &area, image, &crop);
		if (!have_crop) {
			reply(invocation, PORTAL_RESPONSE_OTHER, NULL);
			return;
		}
	}

	path = screenshot_path(&error);
	if (path == NULL || !write_png(image, have_crop ? &crop : NULL, path,
	                               &error)) {
		g_warning("screenshot: %s", error->message);
		reply(invocation, PORTAL_RESPONSE_OTHER, NULL);
		return;
	}
	uri = g_filename_to_uri(path, NULL, NULL);

	g_variant_builder_init(&results, G_VARIANT_TYPE_VARDICT);
	g_variant_builder_add(&results, "{sv}", "uri",
	                      g_variant_new_string(uri));
	reply(invocation, PORTAL_RESPONSE_SUCCESS, &results);
	g_message("screenshot: '%s' took %s%s into %s", app_id, source->title,
	          use_area ? " (an area)" : "", path);
}

static void
handle_pick_color(PortalScreenshot *self, GVariant *params,
                  GDBusMethodInvocation *invocation)
{
	const gchar *handle;
	const gchar *app_id;
	const gchar *parent_window;
	g_autoptr(GPtrArray) sources = NULL;
	g_autoptr(PortalCaptureImage) image = NULL;
	g_autoptr(GError) error = NULL;
	const PortalCaptureSource *output;
	PortalChooserResult result;
	GVariantBuilder results;
	Rect point;
	Rect px;
	guint8 r = 0;
	guint8 g = 0;
	guint8 b = 0;

	g_variant_get(params, "(&o&s&s@a{sv})", &handle, &app_id,
	              &parent_window, NULL);

	result = pick_point(&point);
	if (result == PORTAL_CHOOSER_CANCELLED) {
		reply(invocation, PORTAL_RESPONSE_CANCELLED, NULL);
		return;
	}
	if (result != PORTAL_CHOOSER_CHOSEN) {
		g_message("screenshot: no way to pick a point for '%s'", app_id);
		reply(invocation, PORTAL_RESPONSE_OTHER, NULL);
		return;
	}
	sources = portal_capture_list_sources(self->capture, FALSE, TRUE);
	output = output_at(sources, point.x, point.y);
	if (output == NULL) {
		reply(invocation, PORTAL_RESPONSE_OTHER, NULL);
		return;
	}
	image = portal_capture_grab(self->capture, output, FALSE,
	                            GRAB_TIMEOUT_MS, &error);
	if (image == NULL) {
		g_warning("screenshot: could not capture %s: %s", output->title,
		          error->message);
		reply(invocation, PORTAL_RESPONSE_OTHER, NULL);
		return;
	}
	point.w = 1;
	point.h = 1;
	if (!area_to_pixels(output, &point, image, &px)
	    || !portal_capture_image_pixel(image, (guint32)px.x, (guint32)px.y,
	                                   &r, &g, &b)) {
		reply(invocation, PORTAL_RESPONSE_OTHER, NULL);
		return;
	}

	g_variant_builder_init(&results, G_VARIANT_TYPE_VARDICT);
	g_variant_builder_add(&results, "{sv}", "color",
		g_variant_new("(ddd)", r / 255.0, g / 255.0, b / 255.0));
	reply(invocation, PORTAL_RESPONSE_SUCCESS, &results);
}

static void
ss_method(GDBusConnection *conn, const gchar *sender, const gchar *path,
          const gchar *iface, const gchar *method, GVariant *params,
          GDBusMethodInvocation *invocation, gpointer user_data)
{
	PortalScreenshot *self = user_data;

	(void)conn; (void)sender; (void)path; (void)iface;

	if (g_strcmp0(method, "Screenshot") == 0)
		handle_screenshot(self, params, invocation);
	else if (g_strcmp0(method, "PickColor") == 0)
		handle_pick_color(self, params, invocation);
	else
		g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR,
			G_DBUS_ERROR_UNKNOWN_METHOD, "no such method: %s", method);
}

static GVariant *
ss_get_property(GDBusConnection *conn, const gchar *sender, const gchar *path,
                const gchar *iface, const gchar *name, GError **error,
                gpointer user_data)
{
	PortalScreenshot *self = user_data;

	(void)conn; (void)sender; (void)path; (void)iface;

	if (g_strcmp0(name, "version") == 0)
		return g_variant_new_uint32(SCREENSHOT_VERSION);
	if (g_strcmp0(name, "AvailableTargets") == 0)
		return g_variant_new_uint32(available_targets(self));
	g_set_error(error, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_PROPERTY,
	            "no such property: %s", name);
	return NULL;
}

static const GDBusInterfaceVTable ss_vtable = {
	ss_method, ss_get_property, NULL, { 0 }
};

/* ---------------------------------------------------------------
 * Lifecycle
 * --------------------------------------------------------------- */

PortalScreenshot *
portal_screenshot_new(GDBusConnection *conn, PortalCapture *capture,
                      GError **error)
{
	PortalScreenshot *self;

	g_return_val_if_fail(conn != NULL, NULL);
	g_return_val_if_fail(capture != NULL, NULL);

	if (!portal_capture_available(capture, NULL)) {
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
			"the compositor offers no image-copy-capture");
		return NULL;
	}

	self = g_new0(PortalScreenshot, 1);
	self->conn = g_object_ref(conn);
	self->capture = capture;
	portal_capture_available(capture, &self->can_window);

	if (ss_node_info == NULL)
		ss_node_info = g_dbus_node_info_new_for_xml(ss_introspection_xml,
		                                            NULL);
	self->reg_id = g_dbus_connection_register_object(conn, SCREENSHOT_PATH,
		ss_node_info->interfaces[0], &ss_vtable, self, NULL, error);
	if (self->reg_id == 0) {
		portal_screenshot_free(self);
		return NULL;
	}
	g_message("screenshot: serving outputs%s", self->can_window
	          ? ", windows and areas" : " and areas");
	return self;
}

void
portal_screenshot_free(PortalScreenshot *self)
{
	if (self == NULL)
		return;
	if (self->reg_id != 0 && self->conn != NULL
	    && !g_dbus_connection_is_closed(self->conn))
		g_dbus_connection_unregister_object(self->conn, self->reg_id);
	g_clear_object(&self->conn);
	g_free(self);
}
