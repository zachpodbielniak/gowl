/* Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later */

/*
 * osd -- the centred overlay for a level that just changed.
 *
 * Volume and brightness are the cases: you press a key and want to see
 * where the level landed without reading anything.  A bar toast is the
 * wrong shape for that -- it is a message, it lives at the edge, and it
 * queues behind other messages.  This is one transient pill in the
 * middle of the focused output, replaced rather than queued, and it
 * shows a filled track because the number is not the point.
 *
 * Reached by NAME through ipc_command, the way expo and the screenshot
 * module are:
 *
 *   osd volume 45          a level, 0..100
 *   osd volume 45 muted    a level with a state word
 *   osd brightness 70
 *   osd show Something     a message with no level
 *
 * so a keybind, `gowl osd ...' and cmacs all reach it the same way and
 * none of them has to know this module exists.  It renders nothing when
 * it is not loaded, which is the right failure for decoration.
 */

#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "gowl-osd"
#include "core/gowl-core-private.h"
#include "core/gowl-frame-sink.h"
#include "interfaces/gowl-startup-handler.h"
#include "interfaces/gowl-ipc-handler.h"
#include <cairo.h>
#include <math.h>
#include <string.h>

#define OSD_WIDTH      (260)
#define OSD_HEIGHT     (84)
#define OSD_RADIUS     (14)
#define OSD_TIMEOUT_MS (1400)

typedef struct {
	GowlModule parent;
	GowlCompositor *compositor;
	struct wlr_scene_buffer *pill;
	struct wl_event_source *timer;
	struct wl_listener display_destroy;
} GowlOsd;

typedef struct { GowlModuleClass parent; } GowlOsdClass;

static void osd_startup_init(GowlStartupHandlerInterface *iface);
static void osd_ipc_init(GowlIpcHandlerInterface *iface);

G_DEFINE_TYPE_WITH_CODE(GowlOsd, gowl_osd, GOWL_TYPE_MODULE,
	G_IMPLEMENT_INTERFACE(GOWL_TYPE_STARTUP_HANDLER, osd_startup_init)
	G_IMPLEMENT_INTERFACE(GOWL_TYPE_IPC_HANDLER, osd_ipc_init))

static void
osd_hide(GowlOsd *self)
{
	if (self->pill != NULL) {
		wlr_scene_node_destroy(&self->pill->node);
		self->pill = NULL;
	}
}

static int
osd_expire(void *data)
{
	osd_hide(data);
	return 0;
}

/* The overlay is decoration: a click goes to whatever is beneath it,
   not to a rectangle that happens to be in the way. */
static bool
osd_passthrough(struct wlr_scene_buffer *buffer, double *x, double *y)
{
	(void)buffer;
	(void)x;
	(void)y;
	return false;
}

/*
 * Draw the pill: an icon glyph on the left, a label, and a track when
 * there is a level to show.
 *
 * @fraction is negative for "no level", which is how a plain message
 * gets the same pill without an empty bar under it.
 */
static void
osd_render(GowlOsd *self, GowlMonitor *mon, const gchar *glyph,
           const gchar *label, gdouble fraction)
{
	GowlCompositor *comp = self->compositor;
	cairo_surface_t *surface;
	cairo_t *cr;
	struct wlr_buffer *buffer;
	gdouble scale;
	gint w = OSD_WIDTH, h = OSD_HEIGHT;

	if (comp == NULL || mon == NULL)
		return;

	scale = (mon->wlr_output != NULL && mon->wlr_output->scale > 0.0f)
		? (gdouble)mon->wlr_output->scale : 1.0;

	surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
		(gint)ceil(w * scale), (gint)ceil(h * scale));
	cr = cairo_create(surface);
	cairo_scale(cr, scale, scale);

	/* Rounded ground, matching the layout indicator's so the two read
	   as the same family of transient overlay. */
	cairo_new_sub_path(cr);
	cairo_arc(cr, w - OSD_RADIUS, OSD_RADIUS, OSD_RADIUS - 1,
	          -G_PI / 2, 0);
	cairo_arc(cr, w - OSD_RADIUS, h - OSD_RADIUS, OSD_RADIUS - 1,
	          0, G_PI / 2);
	cairo_arc(cr, OSD_RADIUS, h - OSD_RADIUS, OSD_RADIUS - 1,
	          G_PI / 2, G_PI);
	cairo_arc(cr, OSD_RADIUS, OSD_RADIUS, OSD_RADIUS - 1,
	          G_PI, 3 * G_PI / 2);
	cairo_close_path(cr);
	cairo_set_source_rgba(cr, 0.08, 0.09, 0.12, 0.96);
	cairo_fill_preserve(cr);
	cairo_set_source_rgba(cr, comp->focus_color[0], comp->focus_color[1],
	                      comp->focus_color[2], 0.85);
	cairo_set_line_width(cr, 1.5);
	cairo_stroke(cr);

	/* Icon.  A Nerd Font glyph if the theme has one; the label alone
	   still reads if it does not, so a missing font costs decoration
	   rather than information. */
	cairo_select_font_face(cr, "sans", CAIRO_FONT_SLANT_NORMAL,
	                       CAIRO_FONT_WEIGHT_NORMAL);
	cairo_set_font_size(cr, 26);
	cairo_set_source_rgb(cr, 0.94, 0.95, 0.98);
	cairo_move_to(cr, 22, 44);
	cairo_show_text(cr, glyph != NULL ? glyph : "");

	cairo_select_font_face(cr, "sans", CAIRO_FONT_SLANT_NORMAL,
	                       CAIRO_FONT_WEIGHT_BOLD);
	cairo_set_font_size(cr, 15);
	cairo_move_to(cr, 64, 38);
	cairo_show_text(cr, label != NULL ? label : "");

	if (fraction >= 0.0) {
		gdouble track_x = 64, track_w = w - 64 - 24;
		gdouble track_y = 52, track_h = 8;
		gdouble filled = CLAMP(fraction, 0.0, 1.0) * track_w;

		cairo_set_source_rgba(cr, 1, 1, 1, 0.16);
		cairo_rectangle(cr, track_x, track_y, track_w, track_h);
		cairo_fill(cr);

		if (filled > 0.0) {
			cairo_set_source_rgba(cr, comp->focus_color[0],
			                      comp->focus_color[1],
			                      comp->focus_color[2], 0.95);
			cairo_rectangle(cr, track_x, track_y, filled, track_h);
			cairo_fill(cr);
		}
	}

	cairo_destroy(cr);
	cairo_surface_flush(surface);

	buffer = gowl_raw_buffer_create(cairo_image_surface_get_data(surface),
		cairo_image_surface_get_width(surface),
		cairo_image_surface_get_height(surface),
		cairo_image_surface_get_stride(surface));
	cairo_surface_destroy(surface);
	if (buffer == NULL)
		return;

	/* Replaced, never queued: the second volume press supersedes the
	   first rather than waiting behind it. */
	if (self->pill == NULL) {
		self->pill = wlr_scene_buffer_create(
			comp->layers[GOWL_SCENE_LAYER_OVERLAY], buffer);
		if (self->pill == NULL) {
			wlr_buffer_drop(buffer);
			return;
		}
		self->pill->point_accepts_input = osd_passthrough;
	} else {
		wlr_scene_buffer_set_buffer(self->pill, buffer);
	}
	wlr_buffer_drop(buffer);

	wlr_scene_buffer_set_dest_size(self->pill, w, h);
	/* Low centre of the usable area: out of the way of what is being
	   read, and where a volume overlay is looked for. */
	wlr_scene_node_set_position(&self->pill->node,
		mon->w.x + (mon->w.width - w) / 2,
		mon->w.y + mon->w.height - h - 96);
	wlr_scene_node_raise_to_top(&self->pill->node);

	if (self->timer != NULL)
		wl_event_source_timer_update(self->timer, OSD_TIMEOUT_MS);
}

/* The glyph and label for a known kind, so a caller passes a number and
   gets something that reads. */
static const gchar *
osd_glyph_for(const gchar *kind, gdouble fraction, const gchar *state)
{
	gboolean off = (state != NULL
	                && (strstr(state, "mute") != NULL
	                    || strstr(state, "off") != NULL));

	if (g_strcmp0(kind, "volume") == 0) {
		if (off || fraction <= 0.0)
			return "\xef\x9a\xa9";           /* volume-off  */
		if (fraction < 0.5)
			return "\xef\x80\xa7";           /* volume-down */
		return "\xef\x80\xa8";                   /* volume-up   */
	}
	if (g_strcmp0(kind, "brightness") == 0)
		return (fraction < 0.5) ? "\xef\x81\x85" : "\xef\x86\x85";
	if (g_strcmp0(kind, "mic") == 0)
		return off ? "\xef\x84\xb1" : "\xef\x84\xb0";
	return "\xef\x81\xaa";                           /* info */
}

static gchar *
osd_handle_command(GowlIpcHandler *handler, const gchar *command,
                   const gchar *args)
{
	GowlOsd *self = (GowlOsd *)handler;
	g_auto(GStrv) argv = NULL;
	const gchar *kind;
	const gchar *state = NULL;
	gdouble fraction = -1.0;
	g_autofree gchar *label = NULL;

	if (command == NULL || self->compositor == NULL)
		return NULL;
	if (g_strcmp0(command, "osd") != 0 && g_strcmp0(command, "osd-show") != 0)
		return NULL;

	if (args == NULL || *args == '\0')
		return g_strdup("ERROR usage: osd <volume|brightness|mic|show> [level] [state]");

	argv = g_strsplit_set(args, " \t", 3);
	kind = argv[0];

	if (g_strcmp0(kind, "show") == 0) {
		/* Everything after the word is the message, level-less. */
		label = g_strdup(args + strlen("show") +
		                 (args[strlen("show")] != '\0' ? 1 : 0));
	} else if (argv[1] != NULL) {
		gdouble level = g_ascii_strtod(argv[1], NULL);

		state = argv[2];
		fraction = CLAMP(level / 100.0, 0.0, 1.0);
		if (state != NULL && *state != '\0')
			label = g_strdup_printf("%s  %d%%", state,
			                        (gint)round(level));
		else
			label = g_strdup_printf("%d%%", (gint)round(level));
	} else {
		return g_strdup("ERROR a level is required");
	}

	osd_render(self, self->compositor->selmon,
	           osd_glyph_for(kind, fraction, state), label, fraction);
	return g_strdup("OK");
}

static void
osd_detach(GowlOsd *self)
{
	osd_hide(self);
	if (self->timer != NULL) {
		wl_event_source_remove(self->timer);
		self->timer = NULL;
	}
	if (self->compositor != NULL) {
		g_object_remove_weak_pointer(G_OBJECT(self->compositor),
		                             (gpointer *)&self->compositor);
		self->compositor = NULL;
	}
	wl_list_remove(&self->display_destroy.link);
	wl_list_init(&self->display_destroy.link);
}

static void
osd_display_destroyed(struct wl_listener *listener, void *data)
{
	GowlOsd *self = wl_container_of(listener, self, display_destroy);

	(void)data;
	osd_detach(self);
}

static void
osd_startup(GowlStartupHandler *handler, gpointer compositor)
{
	GowlOsd *self = (GowlOsd *)handler;

	if (self->compositor == compositor)
		return;
	osd_detach(self);

	self->compositor = compositor;
	g_object_add_weak_pointer(G_OBJECT(compositor),
	                          (gpointer *)&self->compositor);
	self->timer = wl_event_loop_add_timer(
		gowl_compositor_get_event_loop(compositor), osd_expire, self);
	self->display_destroy.notify = osd_display_destroyed;
	wl_display_add_destroy_listener(
		gowl_compositor_get_wl_display(compositor),
		&self->display_destroy);
}

static void osd_startup_init(GowlStartupHandlerInterface *iface)
{
	iface->on_startup = osd_startup;
}

static void osd_ipc_init(GowlIpcHandlerInterface *iface)
{
	iface->handle_command = osd_handle_command;
}

static gboolean osd_activate(GowlModule *self) { (void)self; return TRUE; }
static void osd_deactivate(GowlModule *self) { osd_detach((GowlOsd *)self); }
static const gchar *osd_name(GowlModule *self) { (void)self; return "osd"; }

static void
osd_finalize(GObject *object)
{
	osd_detach((GowlOsd *)object);
	G_OBJECT_CLASS(gowl_osd_parent_class)->finalize(object);
}

static void
gowl_osd_class_init(GowlOsdClass *klass)
{
	GowlModuleClass *module = GOWL_MODULE_CLASS(klass);

	module->activate = osd_activate;
	module->deactivate = osd_deactivate;
	module->get_name = osd_name;
	G_OBJECT_CLASS(klass)->finalize = osd_finalize;
}

static void
gowl_osd_init(GowlOsd *self)
{
	wl_list_init(&self->display_destroy.link);
}

G_MODULE_EXPORT GType gowl_module_register(void) { return gowl_osd_get_type(); }
