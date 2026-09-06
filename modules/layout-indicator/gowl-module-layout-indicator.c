/* Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later */
#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "gowl-layout-indicator"
#include "core/gowl-core-private.h"
#include "core/gowl-frame-sink.h"
#include "interfaces/gowl-startup-handler.h"
#include <cairo.h>
#include <math.h>

typedef struct {
 GowlModule parent;
 GowlCompositor *compositor;
 struct wlr_scene_buffer *toast;
 struct wl_event_source *timer;
 struct wl_listener display_destroy;
 gulong changed;
 gulong requested;
} LayoutIndicator;
typedef struct { GowlModuleClass parent; } LayoutIndicatorClass;
static void startup_init(GowlStartupHandlerInterface *iface);
G_DEFINE_TYPE_WITH_CODE(LayoutIndicator, gowl_layout_indicator, GOWL_TYPE_MODULE,
 G_IMPLEMENT_INTERFACE(GOWL_TYPE_STARTUP_HANDLER, startup_init))

static void hide(LayoutIndicator *self)
{
 if (self->toast != NULL) {
  wlr_scene_node_destroy(&self->toast->node);
  self->toast = NULL;
 }
}
static int expire(void *data)
{
 hide(data);
 return 0;
}
static bool passthrough(struct wlr_scene_buffer *buffer, double *x, double *y)
{
 return false;
}
static const gchar *label_for(const gchar *name)
{
 if (g_str_equal(name, "tile")) return "Tiling";
 if (g_str_equal(name, "scrolling")) return "Scrolling";
 if (g_str_equal(name, "monocle")) return "Monocle";
 if (g_str_equal(name, "float")) return "Floating";
 if (g_str_equal(name, "centeredmaster")) return "Centered master";
 if (g_str_equal(name, "fibonacci")) return "Fibonacci";
 return name;
}
static void show_toast(GowlCompositor *comp, GowlMonitor *mon, const gchar *label,
                    gpointer data)
{
 LayoutIndicator *self = data;
 cairo_surface_t *surface;
 cairo_t *cr;
 struct wlr_buffer *buffer;
 cairo_text_extents_t extents;
 gint width, height = 58;
 gdouble scale;

 if (!gowl_module_get_is_active(GOWL_MODULE(self)) || comp->locked
     || mon->wlr_output == NULL || self->timer == NULL) return;
 /* Render at output scale, while placement stays in logical pixels. */
 scale = MAX(1.0, mon->wlr_output->scale);
 surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
 cr = cairo_create(surface);
 cairo_select_font_face(cr, "sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
 cairo_set_font_size(cr, 18);
 cairo_text_extents(cr, label, &extents);
 width = MIN(MAX(180, (gint)ceil(extents.width) + 70), MAX(1, mon->w.width - 32));
 cairo_destroy(cr);
 cairo_surface_destroy(surface);
 surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
                                       (gint)ceil(width * scale), (gint)ceil(height * scale));
 cr = cairo_create(surface);
 cairo_scale(cr, scale, scale);
 cairo_new_sub_path(cr);
 cairo_arc(cr, width - 12, 12, 11, -G_PI / 2, 0);
 cairo_arc(cr, width - 12, height - 12, 11, 0, G_PI / 2);
 cairo_arc(cr, 12, height - 12, 11, G_PI / 2, G_PI);
 cairo_arc(cr, 12, 12, 11, G_PI, 3 * G_PI / 2);
 cairo_close_path(cr);
 cairo_set_source_rgba(cr, 0.08, 0.09, 0.12, 0.96);
 cairo_fill_preserve(cr);
 cairo_set_source_rgba(cr, comp->focus_color[0], comp->focus_color[1], comp->focus_color[2], 0.85);
 cairo_set_line_width(cr, 1.5);
 cairo_stroke(cr);
 cairo_arc(cr, 24, height / 2, 4, 0, 2 * G_PI);
 cairo_fill(cr);
 cairo_select_font_face(cr, "sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
 cairo_set_font_size(cr, 18);
 cairo_set_source_rgb(cr, 0.94, 0.95, 0.98);
 cairo_move_to(cr, 42, (height - extents.height) / 2 - extents.y_bearing);
 cairo_show_text(cr, label);
 cairo_destroy(cr);
 cairo_surface_flush(surface);
 buffer = gowl_raw_buffer_create(cairo_image_surface_get_data(surface),
  cairo_image_surface_get_width(surface), cairo_image_surface_get_height(surface),
  cairo_image_surface_get_stride(surface));
 cairo_surface_destroy(surface);
 if (buffer == NULL) return;
 if (self->toast == NULL) {
  self->toast = wlr_scene_buffer_create(comp->layers[GOWL_SCENE_LAYER_OVERLAY], buffer);
  self->toast->point_accepts_input = passthrough;
 } else {
  wlr_scene_buffer_set_buffer(self->toast, buffer);
 }
 wlr_buffer_drop(buffer);
 wlr_scene_buffer_set_dest_size(self->toast, width, height);
 wlr_scene_node_set_position(&self->toast->node,
                             mon->w.x + (mon->w.width - width) / 2, mon->w.y + 24);
 wlr_scene_node_raise_to_top(&self->toast->node);
 wl_event_source_timer_update(self->timer, 1200);
}
static void changed(GowlCompositor *comp, GowlMonitor *mon, const gchar *name,
                    gpointer data)
{
 show_toast(comp, mon, label_for(name), data);
}
static void detach(LayoutIndicator *self)
{
 hide(self);
 if (self->timer != NULL) {
  wl_event_source_remove(self->timer);
  self->timer = NULL;
 }
 if (self->compositor != NULL) {
  if (self->changed != 0) g_signal_handler_disconnect(self->compositor, self->changed);
  if (self->requested != 0) g_signal_handler_disconnect(self->compositor, self->requested);
  g_object_remove_weak_pointer(G_OBJECT(self->compositor), (gpointer *)&self->compositor);
  self->compositor = NULL;
 }
 self->changed = 0;
 self->requested = 0;
 wl_list_remove(&self->display_destroy.link);
 wl_list_init(&self->display_destroy.link);
}
static void display_destroyed(struct wl_listener *listener, void *data)
{
 LayoutIndicator *self = wl_container_of(listener, self, display_destroy);
 detach(self);
}
static void startup(GowlStartupHandler *handler, gpointer compositor)
{
 LayoutIndicator *self = (LayoutIndicator *)handler;
 if (self->compositor == compositor) return;
 detach(self);
 self->compositor = compositor;
 g_object_add_weak_pointer(G_OBJECT(compositor), (gpointer *)&self->compositor);
 self->changed = g_signal_connect(compositor, "layout-changed", G_CALLBACK(changed), self);
 self->requested = g_signal_connect(compositor, "toast-requested", G_CALLBACK(show_toast), self);
 self->timer = wl_event_loop_add_timer(gowl_compositor_get_event_loop(compositor), expire, self);
 self->display_destroy.notify = display_destroyed;
 wl_display_add_destroy_listener(gowl_compositor_get_wl_display(compositor), &self->display_destroy);
}
static void startup_init(GowlStartupHandlerInterface *iface) { iface->on_startup = startup; }
static gboolean activate(GowlModule *self) { return TRUE; }
static void deactivate(GowlModule *self) { detach((LayoutIndicator *)self); }
static const gchar *name_of(GowlModule *self) { return "layout-indicator"; }
static void finalize(GObject *object)
{
 detach((LayoutIndicator *)object);
 G_OBJECT_CLASS(gowl_layout_indicator_parent_class)->finalize(object);
}
static void gowl_layout_indicator_class_init(LayoutIndicatorClass *klass)
{
 GowlModuleClass *module = GOWL_MODULE_CLASS(klass);
 module->activate = activate;
 module->deactivate = deactivate;
 module->get_name = name_of;
 G_OBJECT_CLASS(klass)->finalize = finalize;
}
static void gowl_layout_indicator_init(LayoutIndicator *self)
{
 wl_list_init(&self->display_destroy.link);
}
G_MODULE_EXPORT GType gowl_module_register(void) { return gowl_layout_indicator_get_type(); }
