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
 * gowl-module-tabbed.c - Tabbed layout: monocle with a tab strip.
 *
 * Every tiled window on the tag gets the whole area below a strip of
 * tabs, one per window, the focused one highlighted -- i3's tabbed
 * container, herbstluftwm's max+tabs, niri's tabbed column, without a
 * container tree: the tag is the group.  Clicking a tab focuses its
 * window; the strip is drawn by the compositor, so it works for every
 * client, sits under nothing, and costs no protocol.
 *
 * The strip is one wlr_scene_buffer per output, rendered with cairo
 * like the osd pill, kept in the tile layer alongside the windows it
 * labels.  It is redrawn from arrange(), on focus and title changes,
 * and hidden the moment another layout takes the output.
 *
 * Settings under modules: tabbed:
 *   height: the strip's height in logical pixels (default 26)
 *   font: cairo toy font family (default "sans")
 */

#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "gowl-tabbed"

#include <glib-object.h>
#include <gmodule.h>
#include <cairo.h>
#include <string.h>
#include <linux/input-event-codes.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_output.h>

#include "module/gowl-module.h"
#include "module/gowl-module-manager.h"
#include "core/gowl-compositor.h"
#include "core/gowl-core-private.h"
#include "core/gowl-frame-sink.h"
#include "core/gowl-layout-registry.h"
#include "interfaces/gowl-layout-provider.h"
#include "interfaces/gowl-mouse-handler.h"
#include "interfaces/gowl-startup-handler.h"

#define TABBED_SYMBOL "[T]"
#define TABBED_DEFAULT_HEIGHT 26

#define GOWL_TYPE_MODULE_TABBED (gowl_module_tabbed_get_type())
G_DECLARE_FINAL_TYPE(GowlModuleTabbed, gowl_module_tabbed,
                     GOWL, MODULE_TABBED, GowlModule)

/* One output's strip: where it is and which window each tab is. */
typedef struct {
	GowlModuleTabbed        *self;
	GowlMonitor             *monitor;   /* weak */
	struct wlr_scene_buffer *node;
	gint                     x;
	gint                     y;
	gint                     width;
	gint                     height;
	GPtrArray               *tabs;      /* GowlClient*, left to right */
} TabStrip;

struct _GowlModuleTabbed {
	GowlModule       parent_instance;

	GowlCompositor  *compositor;  /* weak */
	gint             height;
	gchar           *font;
	GHashTable      *strips;      /* GowlMonitor* -> TabStrip* */
	gulong           focus_id;
	gulong           title_id;
	gulong           layout_id;
	struct wl_listener display_destroy;
};

static void provider_init (GowlLayoutProviderInterface *iface);
static void mouse_init    (GowlMouseHandlerInterface *iface);
static void startup_init  (GowlStartupHandlerInterface *iface);

G_DEFINE_TYPE_WITH_CODE(GowlModuleTabbed, gowl_module_tabbed, GOWL_TYPE_MODULE,
	G_IMPLEMENT_INTERFACE(GOWL_TYPE_LAYOUT_PROVIDER, provider_init)
	G_IMPLEMENT_INTERFACE(GOWL_TYPE_MOUSE_HANDLER, mouse_init)
	G_IMPLEMENT_INTERFACE(GOWL_TYPE_STARTUP_HANDLER, startup_init))

/* The same test the compositor's VISIBLEON makes, for a module. */
static gboolean
tiled_on(GowlClient *c, GowlMonitor *m)
{
	if (c->mon != m || c->isfloating || c->isfullscreen)
		return FALSE;
	if (c->isoverlay)
		return c->overlay_visible;
	return c->issticky || (c->tags & m->tagset[m->seltags]) != 0;
}

static void
strip_hide(TabStrip *s)
{
	if (s->node != NULL)
		wlr_scene_node_set_enabled(&s->node->node, false);
	g_ptr_array_set_size(s->tabs, 0);
}

static void
on_monitor_gone(gpointer data, GObject *where)
{
	TabStrip *s = (TabStrip *)data;
	(void)where;

	/* The output took its scene nodes with it. */
	s->node = NULL;
	s->monitor = NULL;
	g_hash_table_remove(s->self->strips, where);
}

static void
strip_free(gpointer data)
{
	TabStrip *s = (TabStrip *)data;

	if (s->monitor != NULL)
		g_object_weak_unref(G_OBJECT(s->monitor), on_monitor_gone, s);
	if (s->node != NULL)
		wlr_scene_node_destroy(&s->node->node);
	g_ptr_array_unref(s->tabs);
	g_free(s);
}

static TabStrip *
strip_for(GowlModuleTabbed *self, GowlMonitor *m)
{
	TabStrip *s = (TabStrip *)g_hash_table_lookup(self->strips, m);

	if (s != NULL)
		return s;
	s = g_new0(TabStrip, 1);
	s->self = self;
	s->monitor = m;
	s->tabs = g_ptr_array_new();
	g_object_weak_ref(G_OBJECT(m), on_monitor_gone, s);
	g_hash_table_insert(self->strips, m, s);
	return s;
}

/**
 * strip_render:
 *
 * Draws the tabs into a fresh buffer at the output's scale and puts the
 * strip's node over them.  Titles are clipped to their tab; the
 * focused tab is filled with the focus colour, the rest sit on the
 * same dark ground the osd uses so the strip reads as compositor
 * chrome, not as a window.
 */
static void
strip_render(GowlModuleTabbed *self, TabStrip *s, GowlClient *focused)
{
	GowlCompositor *comp = self->compositor;
	GowlMonitor *m = s->monitor;
	cairo_surface_t *surface;
	cairo_t *cr;
	struct wlr_buffer *buffer;
	gdouble scale;
	guint n = s->tabs->len;
	gdouble tab_w;
	guint i;

	if (comp == NULL || m == NULL || n == 0 || s->width <= 0
	    || comp->layers[GOWL_SCENE_LAYER_TILE] == NULL)
		return; /* no scene yet (or a test without one): nothing to draw on */
	scale = (m->wlr_output != NULL && m->wlr_output->scale > 0.0f)
	        ? (gdouble)m->wlr_output->scale : 1.0;
	surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
		(gint)(s->width * scale + 0.5), (gint)(s->height * scale + 0.5));
	cr = cairo_create(surface);
	cairo_scale(cr, scale, scale);

	cairo_set_source_rgba(cr, 0.08, 0.09, 0.12, 0.96);
	cairo_paint(cr);

	tab_w = (gdouble)s->width / n;
	cairo_select_font_face(cr, self->font, CAIRO_FONT_SLANT_NORMAL,
	                       CAIRO_FONT_WEIGHT_NORMAL);
	cairo_set_font_size(cr, MAX(9.0, s->height * 0.5));
	for (i = 0; i < n; i++) {
		GowlClient *c = (GowlClient *)g_ptr_array_index(s->tabs, i);
		const gchar *title = gowl_client_get_title(c);
		gdouble x0 = i * tab_w;
		cairo_text_extents_t ext;
		cairo_font_extents_t fe;

		if (title == NULL || title[0] == '\0')
			title = gowl_client_get_app_id(c);
		if (title == NULL)
			title = "?";

		if (c == focused) {
			cairo_set_source_rgba(cr, comp->focus_color[0],
			                      comp->focus_color[1],
			                      comp->focus_color[2], 0.9);
			cairo_rectangle(cr, x0, 0, tab_w, s->height);
			cairo_fill(cr);
			cairo_set_source_rgba(cr, 0.05, 0.05, 0.07, 1.0);
		} else {
			/* The separator, then the label */
			cairo_set_source_rgba(cr, 1, 1, 1, 0.12);
			cairo_rectangle(cr, x0 + tab_w - 1, 4, 1, s->height - 8);
			cairo_fill(cr);
			if (c->isurgent)
				cairo_set_source_rgba(cr, 1.0, 0.55, 0.2, 1.0);
			else
				cairo_set_source_rgba(cr, 0.85, 0.86, 0.9, 0.9);
		}

		cairo_save(cr);
		cairo_rectangle(cr, x0 + 8, 0, tab_w - 16, s->height);
		cairo_clip(cr);
		cairo_font_extents(cr, &fe);
		cairo_text_extents(cr, title, &ext);
		/* Centred when it fits, left-aligned when it does not */
		if (ext.x_advance < tab_w - 16)
			cairo_move_to(cr, x0 + (tab_w - ext.x_advance) / 2,
			              (s->height + fe.ascent - fe.descent) / 2);
		else
			cairo_move_to(cr, x0 + 8,
			              (s->height + fe.ascent - fe.descent) / 2);
		cairo_show_text(cr, title);
		cairo_restore(cr);
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

	if (s->node == NULL) {
		s->node = wlr_scene_buffer_create(comp->layers[GOWL_SCENE_LAYER_TILE],
		                                  buffer);
		if (s->node == NULL) {
			wlr_buffer_drop(buffer);
			return;
		}
	} else {
		wlr_scene_buffer_set_buffer(s->node, buffer);
	}
	wlr_buffer_drop(buffer);
	wlr_scene_buffer_set_dest_size(s->node, s->width, s->height);
	wlr_scene_node_set_position(&s->node->node, s->x, s->y);
	wlr_scene_node_set_enabled(&s->node->node, true);
	wlr_scene_node_raise_to_top(&s->node->node);
}

/* Rebuilds the tab list for an output from the compositor's clients,
 * in stacking order as the compositor keeps them. */
static void
strip_collect(GowlModuleTabbed *self, TabStrip *s)
{
	GList *l;

	g_ptr_array_set_size(s->tabs, 0);
	for (l = self->compositor->clients; l != NULL; l = l->next) {
		GowlClient *c = (GowlClient *)l->data;

		if (tiled_on(c, s->monitor))
			g_ptr_array_add(s->tabs, c);
	}
}

static void
arrange(GowlLayoutProvider *p, gpointer monitor, GList *clients, gpointer area_ptr)
{
	GowlModuleTabbed *self = GOWL_MODULE_TABBED(p);
	GowlMonitor *m = (GowlMonitor *)monitor;
	GowlCompositor *comp;
	GowlModuleManager *mgr;
	TabStrip *s;
	GowlClient *focused;
	gint oh = 0;
	gint ov = 0;
	gint x, y, w, h;
	guint i;
	(void)clients;
	(void)area_ptr;

	if (m == NULL || m->compositor == NULL)
		return;
	comp = m->compositor;
	if (self->compositor == NULL)
		self->compositor = comp;

	mgr = gowl_compositor_get_module_manager(comp);
	if (mgr != NULL)
		gowl_module_manager_get_gaps(mgr, (gpointer)m, NULL, NULL, &oh, &ov);
	x = m->w.x + oh;
	y = m->w.y + ov;
	w = m->w.width - 2 * oh;
	h = m->w.height - 2 * ov;

	s = strip_for(self, m);
	strip_collect(self, s);
	if (s->tabs->len == 0) {
		strip_hide(s);
		return;
	}

	s->x = x;
	s->y = y;
	s->width = w;
	s->height = MIN(self->height, h / 2);
	for (i = 0; i < s->tabs->len; i++)
		gowl_compositor_place_client(comp,
			(GowlClient *)g_ptr_array_index(s->tabs, i),
			x, y + s->height, w, h - s->height);

	g_free(m->layout_symbol);
	m->layout_symbol = g_strdup_printf("[T%u]", s->tabs->len);

	focused = gowl_compositor_get_focused_client(comp);
	if (focused != NULL && focused->scene != NULL)
		wlr_scene_node_raise_to_top(&focused->scene->node);
	strip_render(self, s, focused);
}

static const gchar *
symbol_of(GowlLayoutProvider *p)
{
	(void)p;
	return TABBED_SYMBOL;
}

/* Is the tabbed layout the one arranging this output right now? */
static gboolean
owns_monitor(GowlModuleTabbed *self, GowlMonitor *m)
{
	GowlLayoutEntry *entry;

	if (self->compositor == NULL || m == NULL)
		return FALSE;
	entry = gowl_layout_get(self->compositor, m);
	return entry != NULL && entry->provider == GOWL_LAYOUT_PROVIDER(self);
}

/* Focus moved or a title changed: redraw every strip we own; no
 * reflow, the geometry has not changed. */
static void
refresh_all(GowlModuleTabbed *self)
{
	GHashTableIter iter;
	gpointer k;
	gpointer v;
	GowlClient *focused;

	if (self->compositor == NULL)
		return;
	focused = gowl_compositor_get_focused_client(self->compositor);
	g_hash_table_iter_init(&iter, self->strips);
	while (g_hash_table_iter_next(&iter, &k, &v)) {
		TabStrip *s = (TabStrip *)v;

		if (!owns_monitor(self, (GowlMonitor *)k)) {
			strip_hide(s);
			continue;
		}
		strip_collect(self, s);
		if (s->tabs->len == 0)
			strip_hide(s);
		else
			strip_render(self, s, focused);
	}
}

static void
on_focus_changed(GowlCompositor *comp, GowlClient *c, gpointer data)
{
	(void)comp;
	(void)c;
	refresh_all(GOWL_MODULE_TABBED(data));
}

static void
on_layout_changed(GowlCompositor *comp, GowlMonitor *m, const gchar *symbol,
                  gpointer data)
{
	GowlModuleTabbed *self = GOWL_MODULE_TABBED(data);
	TabStrip *s;
	(void)comp;
	(void)symbol;

	/* Another layout took the output: the strip goes with us. */
	s = (TabStrip *)g_hash_table_lookup(self->strips, m);
	if (s != NULL && !owns_monitor(self, m))
		strip_hide(s);
}

/* A left click on a tab focuses its window. */
static gboolean
handle_button(GowlMouseHandler *handler, guint button, guint state,
              guint modifiers)
{
	GowlModuleTabbed *self = GOWL_MODULE_TABBED(handler);
	GHashTableIter iter;
	gpointer k;
	gpointer v;
	gdouble cx;
	gdouble cy;

	if (self->compositor == NULL || state != 1 || button != BTN_LEFT
	    || modifiers != 0 || self->compositor->wlr_cursor == NULL)
		return FALSE;
	cx = self->compositor->wlr_cursor->x;
	cy = self->compositor->wlr_cursor->y;

	g_hash_table_iter_init(&iter, self->strips);
	while (g_hash_table_iter_next(&iter, &k, &v)) {
		TabStrip *s = (TabStrip *)v;
		guint i;

		if (s->node == NULL || !s->node->node.enabled || s->tabs->len == 0)
			continue;
		if (cx < s->x || cx >= s->x + s->width || cy < s->y
		    || cy >= s->y + s->height)
			continue;
		i = (guint)((cx - s->x) * s->tabs->len / s->width);
		if (i >= s->tabs->len)
			i = s->tabs->len - 1;
		gowl_compositor_focus_client(self->compositor,
			(GowlClient *)g_ptr_array_index(s->tabs, i), TRUE);
		return TRUE;
	}
	return FALSE;
}

static gboolean
handle_motion(GowlMouseHandler *handler, gdouble x, gdouble y)
{
	(void)handler;
	(void)x;
	(void)y;
	return FALSE;
}

static gboolean
handle_axis(GowlMouseHandler *handler, guint axis, gdouble delta,
            gint discrete, guint modifiers)
{
	(void)handler;
	(void)axis;
	(void)delta;
	(void)discrete;
	(void)modifiers;
	return FALSE;
}

static void
detach(GowlModuleTabbed *self)
{
	if (self->compositor != NULL) {
		if (self->focus_id != 0)
			g_signal_handler_disconnect(self->compositor, self->focus_id);
		if (self->title_id != 0)
			g_signal_handler_disconnect(self->compositor, self->title_id);
		if (self->layout_id != 0)
			g_signal_handler_disconnect(self->compositor, self->layout_id);
		g_object_remove_weak_pointer(G_OBJECT(self->compositor),
		                             (gpointer *)&self->compositor);
		self->compositor = NULL;
	}
	self->focus_id = self->title_id = self->layout_id = 0;
	g_hash_table_remove_all(self->strips);
	wl_list_remove(&self->display_destroy.link);
	wl_list_init(&self->display_destroy.link);
}

/* The display is going: the scene, and every node in it, goes with it
 * before our deactivate could run. */
static void
on_display_destroy(struct wl_listener *listener, void *data)
{
	GowlModuleTabbed *self = wl_container_of(listener, self, display_destroy);
	GHashTableIter iter;
	gpointer v;
	(void)data;

	g_hash_table_iter_init(&iter, self->strips);
	while (g_hash_table_iter_next(&iter, NULL, &v))
		((TabStrip *)v)->node = NULL;
	detach(self);
}

static void
on_startup(GowlStartupHandler *handler, gpointer compositor)
{
	GowlModuleTabbed *self = GOWL_MODULE_TABBED(handler);

	if (self->compositor == compositor)
		return;
	detach(self);
	self->compositor = GOWL_COMPOSITOR(compositor);
	g_object_add_weak_pointer(G_OBJECT(compositor),
	                          (gpointer *)&self->compositor);
	self->focus_id = g_signal_connect(compositor, "focus-changed",
	                                  G_CALLBACK(on_focus_changed), self);
	self->title_id = g_signal_connect(compositor, "client-title-changed",
	                                  G_CALLBACK(on_focus_changed), self);
	self->layout_id = g_signal_connect(compositor, "layout-changed",
	                                   G_CALLBACK(on_layout_changed), self);
	self->display_destroy.notify = on_display_destroy;
	wl_display_add_destroy_listener(gowl_compositor_get_wl_display(compositor),
	                                &self->display_destroy);
}

static void
configure(GowlModule *mod, gpointer config)
{
	GowlModuleTabbed *self = GOWL_MODULE_TABBED(mod);
	GHashTable *settings = (GHashTable *)config;
	const gchar *val;

	if (settings == NULL)
		return;
	val = (const gchar *)g_hash_table_lookup(settings, "height");
	if (val != NULL)
		self->height = CLAMP((gint)g_ascii_strtoll(val, NULL, 10), 12, 200);
	val = (const gchar *)g_hash_table_lookup(settings, "font");
	if (val != NULL && val[0] != '\0') {
		g_free(self->font);
		self->font = g_strdup(val);
	}
}

static gboolean
activate(GowlModule *mod)
{
	(void)mod;
	return TRUE;
}

static void
deactivate(GowlModule *mod)
{
	detach(GOWL_MODULE_TABBED(mod));
}

static const gchar *
name_of(GowlModule *mod)
{
	(void)mod;
	return "tabbed";
}

static const gchar *
description_of(GowlModule *mod)
{
	(void)mod;
	return "Tabbed layout: every window full-size under a strip of tabs";
}

static void
provider_init(GowlLayoutProviderInterface *iface)
{
	iface->arrange = arrange;
	iface->get_symbol = symbol_of;
}

static void
mouse_init(GowlMouseHandlerInterface *iface)
{
	iface->handle_button = handle_button;
	iface->handle_motion = handle_motion;
	iface->handle_axis = handle_axis;
}

static void
startup_init(GowlStartupHandlerInterface *iface)
{
	iface->on_startup = on_startup;
}

static void
gowl_module_tabbed_finalize(GObject *object)
{
	GowlModuleTabbed *self = GOWL_MODULE_TABBED(object);

	detach(self);
	g_hash_table_unref(self->strips);
	g_free(self->font);
	G_OBJECT_CLASS(gowl_module_tabbed_parent_class)->finalize(object);
}

static void
gowl_module_tabbed_class_init(GowlModuleTabbedClass *klass)
{
	GowlModuleClass *mod_class = GOWL_MODULE_CLASS(klass);

	G_OBJECT_CLASS(klass)->finalize = gowl_module_tabbed_finalize;
	mod_class->activate        = activate;
	mod_class->deactivate      = deactivate;
	mod_class->get_name        = name_of;
	mod_class->get_description = description_of;
	mod_class->configure       = configure;
}

static void
gowl_module_tabbed_init(GowlModuleTabbed *self)
{
	self->height = TABBED_DEFAULT_HEIGHT;
	self->font = g_strdup("sans");
	self->strips = g_hash_table_new_full(g_direct_hash, g_direct_equal,
	                                     NULL, strip_free);
	wl_list_init(&self->display_destroy.link);
}

G_MODULE_EXPORT GType
gowl_module_register(void)
{
	return GOWL_TYPE_MODULE_TABBED;
}
