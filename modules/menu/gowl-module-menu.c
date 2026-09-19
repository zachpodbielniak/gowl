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
 * menu -- the card in the middle of the screen.
 *
 * The list is src/menu: what the rows are, which of them are visible and
 * what happens when one is chosen all live there, and are the same rows
 * cmacs shows through completing-read.  This module is the PIXELS and
 * the KEYBOARD, and nothing else.
 *
 * It draws through barkit -- the same #GowlBarPanel the bar's dropdowns
 * are built from, rendered by the same pass with the same theme.  That
 * is not laziness: a control surface that does not look like the rest of
 * the session reads as a different program's dialog, and every palette,
 * radius and font the user has already set would otherwise have to be
 * re-read and re-applied here, in code that would drift.  The keyboard
 * cursor, the hover wash, the hit rectangles and the scroll indicator
 * all come with it.
 *
 * REACHED BY NAME through ipc_command, the way expo and the screenshot
 * module are:
 *
 *   menu                 toggle at the root
 *   menu-toggle [ROUTE]  toggle, opening at ROUTE
 *   menu-open [ROUTE]    always open (no close-if-visible)
 *   menu-close
 *   menu-refresh         re-read menu.yaml
 *   menu-list [ROUTE]    what is there, one row per line
 *   menu-forget          drop the recent-choices history
 *
 * so a keybind, `gowl menu ...' and cmacs all reach it the same way and
 * none of them has to know this module exists.
 *
 * THE KEYBOARD, once it is up: type to search the whole tree, arrows
 * and Page Up/Down to move, Return or Right to choose, Backspace or
 * Left to go back, Escape to clear the search and then to close.  And
 * Ctrl+h/j/k/l for the same four directions without leaving the home
 * row --- modified because every plain letter goes into the search,
 * which is the point of the card.
 */

#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "gowl-menu-ui"

#include "core/gowl-core-private.h"
#include "core/gowl-compositor.h"
#include "core/gowl-monitor.h"
#include "core/gowl-frame-sink.h"
#include "config/gowl-config.h"
#include "menu/gowl-menu.h"
#include "barkit/gowl-bar-panel.h"
#include "barkit/gowl-bar-panel-render.h"
#include "barkit/gowl-bar-theme.h"
#include "barkit/gowl-bar-icon.h"
#include "interfaces/gowl-ipc-handler.h"
#include "interfaces/gowl-keybind-handler.h"
#include "interfaces/gowl-mouse-handler.h"
#include "interfaces/gowl-startup-handler.h"
#include "interfaces/gowl-shutdown-handler.h"

#include <cairo.h>
#include <pango/pangocairo.h>
#include <math.h>
#include <string.h>

#include <errno.h>
#include <unistd.h>
#include <sys/eventfd.h>

#include <linux/input-event-codes.h>
#include <xkbcommon/xkbcommon.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_scene.h>

#include <gmodule.h>

/* How far Page Up and Page Down move.  Six rows is about a third of a
 * full card, which is the distance at which paging is faster than
 * holding the arrow and still leaves something recognisable on screen. */
#define MENU_PAGE (6)

/* The longest filter worth typing.  Past this the search is not
 * narrowing anything and the string stops fitting in the header. */
#define MENU_FILTER_MAX (64)

#define GOWL_TYPE_MODULE_MENU (gowl_module_menu_get_type())
G_DECLARE_FINAL_TYPE(GowlModuleMenu, gowl_module_menu, GOWL, MODULE_MENU,
                     GowlModule)

struct _GowlModuleMenu {
	GowlModule       parent_instance;
	GowlCompositor  *compositor;        /* weak pointer */
	struct wl_listener display_destroy;

	GowlMenu        *menu;
	GowlBarTheme    *theme;

	gboolean         open;
	GowlMonitor     *monitor;           /* unowned; where it is drawn */
	gchar           *route;             /* what is open */
	GString         *filter;
	GPtrArray       *rows;              /* GowlMenuRow *, what is drawn */

	struct wlr_scene_buffer *node;

	/*
	 * ICONS ARE RESOLVED ON A THREAD OF THEIR OWN, and this is not a
	 * refinement.  Turning an application's icon name into pixels means
	 * finding the file and decoding it: measured on an ordinary desktop
	 * that is tens of milliseconds each even with barkit's index, and
	 * far more on a cold page cache.  A dozen rows of that on the
	 * compositor thread is a dozen rows of frozen desktop -- and it
	 * happens while somebody is TYPING, when every keystroke brings new
	 * rows into view.
	 *
	 * So the render draws whatever is already in @icons and asks for
	 * what is not.  The worker answers into @results and pokes
	 * @wake_fd; the compositor thread drains it through the Wayland
	 * event loop and redraws.  Rows show their glyph for a frame or two
	 * and then their real icon, which is what every other application
	 * list does and nobody notices.
	 */
	GHashTable      *icons;             /* themed name -> cairo_surface_t* */
	GHashTable      *asked;             /* name -> requested already */
	GAsyncQueue     *requests;          /* gchar *, or NULL to stop */
	GAsyncQueue     *results;           /* GowlMenuIcon * */
	GThread         *worker;
	int              wake_fd;
	struct wl_event_source *wake_source;

	GArray          *hits;              /* GowlBarHitRect */
	GArray          *item_rows;         /* panel item -> row, or -1 */
	GArray          *row_items;         /* row -> panel item */
	gint             surf_x, surf_y;    /* where the surface landed */
	gint             frame_x, frame_y;  /* the card inside the surface */
	gint             frame_w, frame_h;
	gint             header_h;          /* the search box and title */
	gint             list_h;            /* the rows' clip below it */
	gint             content_h;
	gint             scroll;
	gint             cursor;            /* index into rows, or -1 */
	gint             hover;             /* index into rows, or -1 */
	guint            n_recent;          /* rows at the top from history */
	guint            n_special;         /* rows made from the typed text */

	/* The entrance: a fade and a short rise, on the compositor's loop. */
	struct wl_event_source *anim_timer;
	gint64           anim_start;
	gdouble          anim_alpha;
	gint             anim_dy;

	/* The look, out of modules.menu */
	gint             want_width;
	gint             want_height;
	gint             icon_px;           /* icons are loaded at this size */
	gboolean         animate;
	gboolean         show_hints;
	gint             recent_max;
};

static void menu_ipc_init(GowlIpcHandlerInterface *iface);
static void menu_key_init(GowlKeybindHandlerInterface *iface);
static void menu_mouse_init(GowlMouseHandlerInterface *iface);
static void menu_startup_init(GowlStartupHandlerInterface *iface);
static void menu_shutdown_init(GowlShutdownHandlerInterface *iface);

G_DEFINE_TYPE_WITH_CODE(GowlModuleMenu, gowl_module_menu, GOWL_TYPE_MODULE,
	G_IMPLEMENT_INTERFACE(GOWL_TYPE_IPC_HANDLER, menu_ipc_init)
	G_IMPLEMENT_INTERFACE(GOWL_TYPE_KEYBIND_HANDLER, menu_key_init)
	G_IMPLEMENT_INTERFACE(GOWL_TYPE_MOUSE_HANDLER, menu_mouse_init)
	G_IMPLEMENT_INTERFACE(GOWL_TYPE_STARTUP_HANDLER, menu_startup_init)
	G_IMPLEMENT_INTERFACE(GOWL_TYPE_SHUTDOWN_HANDLER, menu_shutdown_init))

static void menu_render(GowlModuleMenu *self);
static void menu_anim_stop(GowlModuleMenu *self);

/* ── The look, out of the config ─────────────────────────────────── */

static gint
config_int(GHashTable *cfg, const gchar *key, gint fallback)
{
	const gchar *value = cfg != NULL ? g_hash_table_lookup(cfg, key) : NULL;

	if (value == NULL || *value == '\0')
		return fallback;
	return (gint)g_ascii_strtoll(value, NULL, 10);
}

static gboolean
config_bool(GHashTable *cfg, const gchar *key, gboolean fallback)
{
	const gchar *value = cfg != NULL ? g_hash_table_lookup(cfg, key) : NULL;

	if (value == NULL || *value == '\0')
		return fallback;
	return g_ascii_strcasecmp(value, "true") == 0
	       || g_ascii_strcasecmp(value, "yes") == 0
	       || g_ascii_strcasecmp(value, "on") == 0
	       || g_strcmp0(value, "1") == 0;
}

/*
 * Rebuild the theme from the session palette.
 *
 * Everything visual comes from here, which is why there are three
 * settings in this module and not thirty: the card is the bar's panel,
 * so `palette:' and the bar's own theme keys already describe it.
 */
static void
menu_read_style(GowlModuleMenu *self)
{
	GowlConfig *config;
	GHashTable *cfg;
	GowlPalette *palette;
	const gchar *font;

	if (self->compositor == NULL)
		return;
	config = gowl_compositor_get_config(self->compositor);
	if (config == NULL)
		return;

	if (self->theme == NULL)
		self->theme = gowl_bar_theme_new();
	palette = gowl_config_get_palette(config);
	if (palette != NULL)
		gowl_bar_theme_apply_palette(self->theme, palette);

	cfg = gowl_config_get_module_config(config, "menu");
	self->want_width  = config_int(cfg, "width", 0);
	self->want_height = config_int(cfg, "max-height", 0);
	self->animate     = config_bool(cfg, "animate", TRUE);
	self->show_hints  = config_bool(cfg, "hints", TRUE);
	self->recent_max  = CLAMP(config_int(cfg, "recent", 5), 0, 20);

	/*
	 * Icons are loaded ONCE at a fixed size and scaled to fit the row,
	 * rather than reloaded whenever the row height changes: an SVG
	 * rasterised at 32 and drawn at 22 is indistinguishable from one
	 * rasterised at 22, and reloading on a theme change would throw
	 * away the whole cache to gain nothing anybody can see.
	 */
	{
		gint row_h = gowl_bar_theme_metric(self->theme,
		                                   GOWL_BAR_METRIC_ROW_HEIGHT);

		self->icon_px = config_int(cfg, "icon-size",
		                           MAX(24, (row_h * 3) / 4));
	}

	font = cfg != NULL ? g_hash_table_lookup(cfg, "font") : NULL;
	if (font != NULL && *font != '\0')
		gowl_bar_theme_set_font(self->theme, font);
}

/* ── Icons, off the compositor thread ────────────────────────────── */

typedef struct {
	gchar           *name;
	cairo_surface_t *surface;   /* NULL when nothing resolved */
} GowlMenuIcon;

static void
menu_icon_free(gpointer data)
{
	GowlMenuIcon *icon = data;

	if (icon == NULL)
		return;
	g_free(icon->name);
	g_clear_pointer(&icon->surface, cairo_surface_destroy);
	g_free(icon);
}

/*
 * The worker.
 *
 * Resolves one name at a time, for as long as the queue gives it names,
 * and hands back ownership of whatever it made.  It touches nothing the
 * compositor thread owns: barkit's index has a lock of its own, and the
 * only thing crossing back is a surface nobody else has seen yet.
 */
static gpointer
menu_icon_worker(gpointer data)
{
	GowlModuleMenu *self = data;

	for (;;) {
		g_autofree gchar *name = g_async_queue_pop(self->requests);
		GowlMenuIcon *icon;
		g_autofree gchar *path = NULL;

		/* The empty string is the sentinel: a NULL cannot be put in
		 * a GAsyncQueue. */
		if (name == NULL || *name == '\0')
			return NULL;

		path = gowl_bar_icon_find_file(name, NULL, self->icon_px);
		icon = g_new0(GowlMenuIcon, 1);
		icon->name = g_strdup(name);
		icon->surface = path != NULL
			? gowl_bar_icon_from_file(path, self->icon_px) : NULL;

		g_async_queue_push(self->results, icon);
		if (self->wake_fd >= 0) {
			guint64 one = 1;
			ssize_t written;

			do {
				written = write(self->wake_fd, &one, sizeof one);
			} while (written < 0 && errno == EINTR);
		}
	}
	return NULL;
}

/*
 * Drain what the worker has finished, on the compositor thread.
 *
 * A name that resolved to nothing is still recorded, so it is asked for
 * once rather than on every repaint -- the misses are the expensive
 * ones, since they are the names no theme can answer.
 */
static int
menu_icons_ready(int fd, uint32_t mask, void *data)
{
	GowlModuleMenu *self = data;
	guint64 drained;
	gboolean any = FALSE;
	ssize_t got;

	do {
		got = read(fd, &drained, sizeof drained);
	} while (got < 0 && errno == EINTR);

	for (;;) {
		GowlMenuIcon *icon = g_async_queue_try_pop(self->results);

		if (icon == NULL)
			break;
		g_hash_table_replace(self->icons, g_strdup(icon->name),
		                     g_steal_pointer(&icon->surface));
		menu_icon_free(icon);
		any = TRUE;
	}

	if (any && self->open)
		menu_render(self);
	return 0;
}

/*
 * The icon for a name, if it has arrived.
 *
 * Never blocks: a name nobody has asked for yet is asked for and %NULL
 * comes back, which the row draws as its glyph until the answer does.
 */
static cairo_surface_t *
menu_icon_for(GowlModuleMenu *self, const gchar *name)
{
	cairo_surface_t *have = NULL;

	if (name == NULL || *name == '\0' || self->icons == NULL)
		return NULL;
	if (g_hash_table_lookup_extended(self->icons, name, NULL,
	                                 (gpointer *)&have))
		return have;

	if (self->requests != NULL
	    && !g_hash_table_contains(self->asked, name)) {
		g_hash_table_add(self->asked, g_strdup(name));
		g_async_queue_push(self->requests, g_strdup(name));
	}
	return NULL;
}

/* ── Rows ────────────────────────────────────────────────────────── */

static void
menu_reload_rows(GowlModuleMenu *self)
{
	guint i;

	g_clear_pointer(&self->rows, g_ptr_array_unref);
	self->n_recent  = 0;
	self->n_special = 0;
	if (self->filter->len > 0) {
		self->rows = gowl_menu_search(self->menu, self->compositor,
		                              self->filter->str);
		/* The model puts the typed-text rows first; count them so
		 * the panel can head the rest. */
		for (i = 0; self->rows != NULL && i < self->rows->len; i++) {
			const GowlMenuRow *row = g_ptr_array_index(self->rows, i);

			if (row->route == NULL
			    || !(g_str_has_prefix(row->route, "calc:")
			         || g_str_has_prefix(row->route, "run:")
			         || g_str_has_prefix(row->route, "open:")))
				break;
			self->n_special++;
		}
	} else {
		self->rows = gowl_menu_list(self->menu, self->compositor,
		                            self->route);
		/*
		 * At the root, what was chosen recently comes first: the
		 * menu is opened for the same three things most days, and
		 * a list that has learnt that is a list you do not type
		 * into.
		 */
		if (self->recent_max > 0
		    && g_strcmp0(self->route, "root") == 0) {
			g_autoptr(GPtrArray) recent = gowl_menu_recent(
				self->menu, self->compositor,
				(guint)self->recent_max);
			guint n = recent->len;

			for (i = n; i > 0; i--) {
				g_ptr_array_insert(self->rows, 0,
					g_ptr_array_steal_index(recent, i - 1));
			}
			self->n_recent = n;
		}
	}
	/*
	 * The cursor starts on the first row that can be chosen.  Starting
	 * on a disabled one means the first Return does nothing, which
	 * reads as the menu being broken rather than as that row being
	 * unavailable.
	 */
	self->cursor = -1;
	self->hover  = -1;
	self->scroll = 0;
}

/* The first selectable row at or after @from, walking in @direction. */
static gint
menu_step(GowlModuleMenu *self, gint from, gint direction)
{
	gint n = self->rows != NULL ? (gint)self->rows->len : 0;
	gint i;
	gint tried;

	if (n == 0)
		return -1;
	i = from;
	for (tried = 0; tried < n; tried++) {
		GowlMenuRow *row;

		i += direction;
		if (i < 0)
			i = n - 1;
		if (i >= n)
			i = 0;
		row = g_ptr_array_index(self->rows, i);
		if (!row->disabled)
			return i;
	}
	return -1;
}

/* ── Opening and closing ─────────────────────────────────────────── */

static void
menu_hide(GowlModuleMenu *self)
{
	menu_anim_stop(self);
	if (self->node != NULL) {
		wlr_scene_node_destroy(&self->node->node);
		self->node = NULL;
	}
	self->open    = FALSE;
	if (self->monitor != NULL) {
		g_object_remove_weak_pointer(G_OBJECT(self->monitor),
		                             (gpointer *)&self->monitor);
		self->monitor = NULL;
	}
	self->hover   = -1;
	self->cursor  = -1;
	g_string_truncate(self->filter, 0);
	g_clear_pointer(&self->rows, g_ptr_array_unref);
	if (self->hits != NULL)
		g_array_set_size(self->hits, 0);
}

static void
menu_open_route(GowlModuleMenu *self, const gchar *route)
{
	g_autofree gchar *resolved = NULL;

	if (self->compositor == NULL)
		return;

	resolved = gowl_menu_resolve(self->menu, route);
	if (!gowl_menu_has_route(self->menu, resolved)) {
		g_message("menu: no such route `%s'", resolved);
		return;
	}
	/*
	 * A route that names an ACTION runs it instead of opening a list
	 * with nothing in it.  `gowl menu summon system.lock' means lock,
	 * and an alias that happens to point at a leaf should not have to
	 * be a different kind of thing to say.
	 */
	if (!gowl_menu_is_submenu(self->menu, resolved)) {
		g_autofree gchar *next = NULL;

		if (gowl_menu_activate(self->menu, self->compositor, resolved,
		                       &next) != GOWL_MENU_RESULT_OPEN) {
			menu_hide(self);
			return;
		}
		g_free(resolved);
		resolved = g_steal_pointer(&next);
	}

	g_free(self->route);
	self->route = g_steal_pointer(&resolved);
	g_string_truncate(self->filter, 0);

	/*
	 * Somewhere to draw, or nothing at all.  Open with no output the
	 * card is invisible and the key handler still swallows every key:
	 * a keyboard that has stopped working, with nothing on screen to
	 * say why.  Held weakly, so an output unplugged while the card is
	 * up reads as NULL rather than as freed memory.
	 */
	{
		GowlMonitor *mon = gowl_compositor_get_selected_monitor(
			self->compositor);

		if (mon == NULL) {
			g_message("menu: no output to draw on");
			menu_hide(self);
			return;
		}
		if (self->monitor != mon) {
			if (self->monitor != NULL)
				g_object_remove_weak_pointer(
					G_OBJECT(self->monitor),
					(gpointer *)&self->monitor);
			self->monitor = mon;
			g_object_add_weak_pointer(G_OBJECT(mon),
			                          (gpointer *)&self->monitor);
		}
	}

	self->open = TRUE;
	menu_read_style(self);
	menu_reload_rows(self);
	self->cursor = menu_step(self, -1, 1);
	menu_render(self);
}

static void
menu_go_up(GowlModuleMenu *self)
{
	g_autofree gchar *up = gowl_menu_get_parent(self->menu, self->route);

	/* At the root there is nowhere up to go, and the card stays: the
	 * key that closes it is Escape (or the one that opened it). */
	if (up == NULL)
		return;
	menu_open_route(self, up);
}

static void
menu_activate_index(GowlModuleMenu *self, gint index)
{
	GowlMenuRow *row;
	g_autofree gchar *next = NULL;
	GowlMenuResult result;

	if (self->rows == NULL || index < 0 || index >= (gint)self->rows->len)
		return;
	row = g_ptr_array_index(self->rows, index);
	if (row->disabled)
		return;

	result = gowl_menu_activate(self->menu, self->compositor, row->route,
	                            &next);
	switch (result) {
	case GOWL_MENU_RESULT_OPEN:
		menu_open_route(self, next);
		break;
	case GOWL_MENU_RESULT_RAN_OPEN:
		/*
		 * Stay up, and re-list: the row that just ran usually
		 * changes its own tick (a backdrop, a toggle), and a menu
		 * still showing the previous answer is worse than one that
		 * closed.
		 */
		menu_reload_rows(self);
		self->cursor = index < (gint)self->rows->len ? index
		                                            : menu_step(self, -1, 1);
		menu_render(self);
		break;
	case GOWL_MENU_RESULT_RAN:
		menu_hide(self);
		break;
	case GOWL_MENU_RESULT_NONE:
	default:
		/* Nothing happened, so nothing changes -- including the
		 * menu staying open, which is the only way to tell. */
		break;
	}
}

/* ── Drawing ─────────────────────────────────────────────────────── */

/* The card takes pointer input, but through the mouse handler rather
 * than through the scene: a scene buffer that accepts input would take
 * focus away from the window underneath, which the menu must not do --
 * every action it runs acts on the focused window. */
static bool
menu_passthrough(struct wlr_scene_buffer *buffer, double *x, double *y)
{
	(void)buffer;
	(void)x;
	(void)y;
	return false;
}

/*
 * Where in the tree the open list is, as a line of text.
 *
 * Not the route: `style.backdrop' is an identifier and the header wants
 * the words the rows were labelled with.  At the root there is no path
 * to show, and the space is left for the search box's own placeholder.
 */
static gchar *
menu_breadcrumb(GowlModuleMenu *self)
{
	GString *out = g_string_new(NULL);
	GPtrArray *parts = g_ptr_array_new_with_free_func(g_free);
	g_autofree gchar *walk = g_strdup(self->route);
	guint i;

	while (walk != NULL && g_strcmp0(walk, "root") != 0) {
		gchar *up = gowl_menu_get_parent(self->menu, walk);

		g_ptr_array_add(parts, gowl_menu_get_title(self->menu, walk));
		g_free(walk);
		walk = up;
	}
	/* Deepest first on the way up, so it is walked back down here. */
	for (i = parts->len; i > 0; i--) {
		if (out->len > 0)
			g_string_append(out, "  \xe2\x80\xba  ");
		g_string_append(out, (const gchar *)g_ptr_array_index(parts, i - 1));
	}
	g_ptr_array_free(parts, TRUE);
	return g_string_free(out, FALSE);
}

/* Whether a row was made from the typed text rather than the tree. */
static gboolean
row_is_special(const GowlMenuRow *row)
{
	return row->route != NULL
	       && (g_str_has_prefix(row->route, "calc:")
	           || g_str_has_prefix(row->route, "run:")
	           || g_str_has_prefix(row->route, "open:"));
}

/* One panel item appended, and which row (or none) it stands for. */
static void
menu_note_item(GowlModuleMenu *self, gint row)
{
	g_array_append_val(self->item_rows, row);
	if (row >= 0) {
		gint item = (gint)self->item_rows->len - 1;

		g_array_append_val(self->row_items, item);
	}
}

/* The panel item a row is drawn as, or -1. */
static gint
menu_item_of_row(GowlModuleMenu *self, gint row)
{
	if (self->row_items == NULL || row < 0
	    || row >= (gint)self->row_items->len)
		return -1;
	return g_array_index(self->row_items, gint, row);
}

/* The row a panel item stands for, or -1. */
static gint
menu_row_of_item(GowlModuleMenu *self, gint item)
{
	if (self->item_rows == NULL || item < 0
	    || item >= (gint)self->item_rows->len)
		return -1;
	return g_array_index(self->item_rows, gint, item);
}

/*
 * Build the panel the renderer draws: the rows, in sections.
 *
 * The header is not in it -- the search box and the breadcrumb are
 * drawn by this module above the panel, because barkit's hero is a
 * label and a search field is not: it needs a caret, a placeholder,
 * and text that is not tracked into capitals.
 */
static GowlBarPanel *
menu_build_panel(GowlModuleMenu *self)
{
	GowlBarPanel *panel = gowl_bar_panel_new();
	g_autoptr(GArray) positions = NULL;
	gboolean searching = self->filter->len > 0;
	guint i;

	g_array_set_size(self->item_rows, 0);
	g_array_set_size(self->row_items, 0);

	if (self->rows == NULL || self->rows->len == 0) {
		gowl_bar_panel_add_spacer(panel, 6);
		menu_note_item(self, -1);
		gowl_bar_panel_add_label(panel, searching
			? "Nothing matches.  Fewer letters find more; "
			  "= starts a sum, ! runs a command."
			: "Nothing here");
		menu_note_item(self, -1);
		return panel;
	}

	positions = g_array_new(FALSE, FALSE, sizeof(guint));

	for (i = 0; i < self->rows->len; i++) {
		GowlMenuRow *row = g_ptr_array_index(self->rows, i);
		GowlBarPanelItem *item;

		/* Section headers, at the seams: the recent choices before
		 * the tree at the root, and the typed-text rows before the
		 * results while searching. */
		if (!searching && self->n_recent > 0) {
			if (i == 0) {
				gowl_bar_panel_add_section(panel, "Recent");
				menu_note_item(self, -1);
			} else if (i == self->n_recent) {
				gowl_bar_panel_add_section(panel, "Everything");
				menu_note_item(self, -1);
			}
		}
		if (searching && self->n_special > 0 && i == self->n_special) {
			gowl_bar_panel_add_section(panel, "Results");
			menu_note_item(self, -1);
		}

		item = gowl_bar_panel_add_row(panel, row->route, row->icon,
		                              row->label, row->detail);
		menu_note_item(self, (gint)i);
		if (row->value != NULL)
			gowl_bar_panel_item_set_value(item, row->value);
		/*
		 * An application's real icon, when the row names one.  The
		 * glyph stays set as the fallback: a theme that has never
		 * heard of the name draws the generic one rather than a gap,
		 * and the column lines up either way.
		 */
		if (row->icon_name != NULL) {
			cairo_surface_t *image = menu_icon_for(self,
				row->icon_name);

			if (image != NULL)
				gowl_bar_panel_item_set_image(item, image);
		}
		/*
		 * The letters that matched, lit up: the model's matcher run
		 * again over the label so what ranked and what is shown are
		 * one thing.  Not for the typed-text rows, whose label IS
		 * the answer.
		 */
		if (searching && !row_is_special(row)
		    && gowl_menu_match(row->label, self->filter->str,
		                       positions) >= 0
		    && positions->len > 0) {
			g_autoptr(GString) spec = g_string_new(NULL);
			guint k;

			for (k = 0; k < positions->len; k++) {
				if (k > 0)
					g_string_append_c(spec, ',');
				g_string_append_printf(spec, "%u",
					g_array_index(positions, guint, k));
			}
			gowl_bar_panel_item_set_prop(item, "match", spec->str);
		}
		/*
		 * One glyph on the right, and the tick wins.  A checked
		 * submenu is rare and "this is the current choice" is the
		 * more useful of the two things to say about it.
		 */
		if (row->checked)
			gowl_bar_panel_item_set_badge(item, "\xe2\x9c\x93");
		else if (row->submenu)
			gowl_bar_panel_item_set_badge(item, "\xe2\x80\xba");
		else if (row_is_special(row))
			gowl_bar_panel_item_set_badge(item, "\xe2\x8f\x8e");
		if (row->disabled)
			gowl_bar_panel_item_set_disabled(item, TRUE);
	}
	return panel;
}

/* A Pango layout in the theme's body font at @scale, bold or not. */
static PangoLayout *
menu_text_layout(GowlModuleMenu *self, cairo_t *cr, gdouble scale,
                 gboolean bold)
{
	PangoLayout *layout = pango_cairo_create_layout(cr);
	PangoFontDescription *desc;
	gint size;

	desc = pango_font_description_from_string(
		gowl_bar_theme_get_font(self->theme));
	size = pango_font_description_get_size(desc);
	if (size <= 0)
		size = 11 * PANGO_SCALE;
	if (pango_font_description_get_size_is_absolute(desc))
		pango_font_description_set_absolute_size(desc, size * scale);
	else
		pango_font_description_set_size(desc, (gint)(size * scale));
	pango_font_description_set_weight(desc,
		bold ? PANGO_WEIGHT_BOLD : PANGO_WEIGHT_NORMAL);
	pango_layout_set_font_description(layout, desc);
	pango_font_description_free(desc);
	return layout;
}

/* The header's height, from the same metrics it is drawn with. */
static void
menu_header_metrics(GowlModuleMenu *self, gint *title_h, gint *box_h,
                    gint *header_h, gint *footer_h)
{
	gint row_h = gowl_bar_theme_metric(self->theme, GOWL_BAR_METRIC_ROW_HEIGHT);
	gint pad_y = gowl_bar_theme_metric(self->theme, GOWL_BAR_METRIC_PAD_Y);
	gint gap   = gowl_bar_theme_metric(self->theme, GOWL_BAR_METRIC_GAP);

	*title_h  = (row_h * 8) / 10;
	*box_h    = (row_h * 11) / 10;
	*header_h = pad_y + *title_h + gap / 2 + *box_h + (pad_y * 3) / 4 + 2;
	*footer_h = self->show_hints ? (row_h * 7) / 10 + gap / 2 : 0;
}

/*
 * The header: what is open, how many rows there are, and the search
 * box -- which is a box, with a caret, whether or not anything has
 * been typed, so the card says "type here" without a word of prose.
 */
static void
menu_draw_header(GowlModuleMenu *self, cairo_t *cr, gint x, gint y,
                 gint width, gint title_h, gint box_h)
{
	gint pad_x = gowl_bar_theme_metric(self->theme, GOWL_BAR_METRIC_PAD_X);
	gint pad_y = gowl_bar_theme_metric(self->theme, GOWL_BAR_METRIC_PAD_Y);
	gint gap   = gowl_bar_theme_metric(self->theme, GOWL_BAR_METRIC_GAP);
	gint radius = gowl_bar_theme_metric(self->theme, GOWL_BAR_METRIC_RADIUS);
	gint inner_w = width - 2 * pad_x;
	gint cy, box_y, text_x;
	PangoLayout *layout;
	PangoRectangle logical;
	g_autofree gchar *title = NULL;
	g_autofree gchar *count = NULL;
	gboolean searching = self->filter->len > 0;

	/* Title on the left, the count on the right. */
	cy = y + pad_y + title_h / 2;
	if (searching) {
		title = g_strdup("Search");
	} else if (g_strcmp0(self->route, "root") == 0) {
		title = g_strdup("Menu");
	} else {
		title = menu_breadcrumb(self);
	}
	count = g_strdup_printf("%u %s",
		self->rows != NULL ? self->rows->len : 0u,
		searching ? "found" : "items");

	layout = menu_text_layout(self, cr, 0.8, FALSE);
	pango_layout_set_text(layout, count, -1);
	pango_layout_get_pixel_extents(layout, NULL, &logical);
	gowl_bar_theme_cairo_set(self->theme, cr, GOWL_BAR_COLOR_MUTED);
	cairo_move_to(cr, x + width - pad_x - logical.width,
	              cy - logical.height / 2);
	pango_cairo_show_layout(cr, layout);
	{
		gint count_w = logical.width;

		pango_layout_set_font_description(layout, NULL);
		g_object_unref(layout);
		layout = menu_text_layout(self, cr, 1.15, TRUE);
		pango_layout_set_text(layout, title, -1);
		pango_layout_set_width(layout,
			(inner_w - count_w - gap) * PANGO_SCALE);
		pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_START);
		pango_layout_get_pixel_extents(layout, NULL, &logical);
		gowl_bar_theme_cairo_set(self->theme, cr, GOWL_BAR_COLOR_TEXT);
		cairo_move_to(cr, x + pad_x, cy - logical.height / 2);
		pango_cairo_show_layout(cr, layout);
		g_object_unref(layout);
	}

	/* The search box. */
	box_y = y + pad_y + title_h + gap / 2;
	gowl_bar_cairo_rounded_rect(cr, x + pad_x, box_y, inner_w, box_h,
	                            MAX(4, radius - 2));
	gowl_bar_theme_cairo_set_alpha(self->theme, cr, GOWL_BAR_COLOR_SURFACE,
	                               searching ? 1.0 : 0.7);
	cairo_fill_preserve(cr);
	gowl_bar_theme_cairo_set_alpha(self->theme, cr,
		searching ? GOWL_BAR_COLOR_ACCENT : GOWL_BAR_COLOR_OVERLAY,
		searching ? 0.9 : 0.6);
	cairo_set_line_width(cr, 1.0);
	cairo_stroke(cr);

	cy = box_y + box_h / 2;
	text_x = x + pad_x + gap;

	/* The magnifier, in the icon font. */
	{
		PangoFontDescription *desc;
		gint glyph_w;

		layout = pango_cairo_create_layout(cr);
		desc = pango_font_description_from_string(
			gowl_bar_theme_get_icon_font(self->theme));
		pango_layout_set_font_description(layout, desc);
		pango_font_description_free(desc);
		pango_layout_set_text(layout, "\xef\x80\x82", -1);
		pango_layout_get_pixel_extents(layout, NULL, &logical);
		glyph_w = logical.width;
		gowl_bar_theme_cairo_set(self->theme, cr,
			searching ? GOWL_BAR_COLOR_ACCENT : GOWL_BAR_COLOR_MUTED);
		cairo_move_to(cr, text_x, cy - logical.height / 2);
		pango_cairo_show_layout(cr, layout);
		g_object_unref(layout);
		text_x += glyph_w + gap;
	}

	/* The text, or what to type; then the caret. */
	layout = menu_text_layout(self, cr, 1.0, FALSE);
	if (searching) {
		pango_layout_set_text(layout, self->filter->str, -1);
		gowl_bar_theme_cairo_set(self->theme, cr, GOWL_BAR_COLOR_TEXT);
	} else {
		pango_layout_set_text(layout,
			"Type to search  \xc2\xb7  = sum  \xc2\xb7  ! run  "
			"\xc2\xb7  a URL or a path", -1);
		gowl_bar_theme_cairo_set(self->theme, cr, GOWL_BAR_COLOR_MUTED);
	}
	pango_layout_set_width(layout,
		(x + width - pad_x - gap - text_x) * PANGO_SCALE);
	pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_START);
	pango_layout_get_pixel_extents(layout, NULL, &logical);
	cairo_move_to(cr, text_x, cy - logical.height / 2);
	pango_cairo_show_layout(cr, layout);
	{
		gint caret_x = text_x + (searching ? logical.width + 2 : 0);
		gint caret_h = MAX(logical.height, box_h / 2);

		gowl_bar_theme_cairo_set(self->theme, cr, GOWL_BAR_COLOR_ACCENT);
		cairo_rectangle(cr, caret_x, cy - caret_h / 2, 2, caret_h);
		cairo_fill(cr);
	}
	g_object_unref(layout);

	/* An accent rule under the header, fading out to the right. */
	{
		cairo_pattern_t *grad;
		const gdouble *c = gowl_bar_theme_color(self->theme,
		                                        GOWL_BAR_COLOR_ACCENT);
		gint rule_y = box_y + box_h + (pad_y * 3) / 4;

		grad = cairo_pattern_create_linear(x + pad_x, 0,
		                                   x + width - pad_x, 0);
		cairo_pattern_add_color_stop_rgba(grad, 0.0, c[0], c[1], c[2], 0.9);
		cairo_pattern_add_color_stop_rgba(grad, 1.0, c[0], c[1], c[2], 0.0);
		cairo_set_source(cr, grad);
		cairo_rectangle(cr, x + pad_x, rule_y, inner_w, 2);
		cairo_fill(cr);
		cairo_pattern_destroy(grad);
	}
}

/* The footer: the keys, in one muted line. */
static void
menu_draw_footer(GowlModuleMenu *self, cairo_t *cr, gint x, gint y,
                 gint width, gint footer_h)
{
	gint pad_x = gowl_bar_theme_metric(self->theme, GOWL_BAR_METRIC_PAD_X);
	PangoLayout *layout;
	PangoRectangle logical;
	const gchar *hint;

	if (footer_h <= 0)
		return;

	if (self->filter->len > 0)
		hint = "\xe2\x86\x91\xe2\x86\x93 move   \xe2\x8f\x8e open   "
		       "esc clear   ctrl+u clear   alt+1..9 pick";
	else if (g_strcmp0(self->route, "root") == 0)
		hint = "\xe2\x86\x91\xe2\x86\x93 move   \xe2\x8f\x8e open   "
		       "type to search   esc close";
	else
		hint = "\xe2\x86\x91\xe2\x86\x93 move   \xe2\x8f\x8e open   "
		       "\xe2\x8c\xab back   esc close";

	layout = menu_text_layout(self, cr, 0.75, FALSE);
	pango_layout_set_text(layout, hint, -1);
	pango_layout_set_width(layout, (width - 2 * pad_x) * PANGO_SCALE);
	pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
	pango_layout_set_alignment(layout, PANGO_ALIGN_CENTER);
	pango_layout_get_pixel_extents(layout, NULL, &logical);
	gowl_bar_theme_cairo_set_alpha(self->theme, cr, GOWL_BAR_COLOR_MUTED,
	                               0.8);
	cairo_move_to(cr, x + pad_x, y + (footer_h - logical.height) / 2);
	pango_cairo_show_layout(cr, layout);
	g_object_unref(layout);
}

/* ── The entrance ────────────────────────────────────────────────── */

#define MENU_ANIM_MS (140)
#define MENU_ANIM_RISE (14)

static void menu_anim_apply(GowlModuleMenu *self);

/* One tick of the fade-and-rise, on the compositor's loop. */
static int
menu_anim_tick(void *data)
{
	GowlModuleMenu *self = data;
	gint64 elapsed = g_get_monotonic_time() - self->anim_start;
	gdouble t = (gdouble)elapsed / (MENU_ANIM_MS * 1000.0);

	if (t >= 1.0 || !self->open) {
		self->anim_alpha = 1.0;
		self->anim_dy = 0;
		menu_anim_apply(self);
		if (self->anim_timer != NULL) {
			wl_event_source_remove(self->anim_timer);
			self->anim_timer = NULL;
		}
		return 0;
	}
	/* Ease out: fast at first, settling at the end. */
	t = 1.0 - (1.0 - t) * (1.0 - t);
	self->anim_alpha = t;
	self->anim_dy = (gint)((1.0 - t) * MENU_ANIM_RISE);
	menu_anim_apply(self);
	wl_event_source_timer_update(self->anim_timer, 16);
	return 0;
}

static void
menu_anim_apply(GowlModuleMenu *self)
{
	if (self->node == NULL)
		return;
	wlr_scene_buffer_set_opacity(self->node, (float)self->anim_alpha);
	wlr_scene_node_set_position(&self->node->node, self->surf_x,
	                            self->surf_y + self->anim_dy);
}

static void
menu_anim_stop(GowlModuleMenu *self)
{
	if (self->anim_timer != NULL) {
		wl_event_source_remove(self->anim_timer);
		self->anim_timer = NULL;
	}
	self->anim_alpha = 1.0;
	self->anim_dy = 0;
}

/* Start the entrance, if the loop and the setting allow one. */
static void
menu_anim_start(GowlModuleMenu *self)
{
	struct wl_event_loop *loop;

	menu_anim_stop(self);
	if (!self->animate || self->compositor == NULL)
		return;
	loop = gowl_compositor_get_event_loop(self->compositor);
	if (loop == NULL)
		return;
	self->anim_timer = wl_event_loop_add_timer(loop, menu_anim_tick, self);
	if (self->anim_timer == NULL)
		return;
	self->anim_start = g_get_monotonic_time();
	self->anim_alpha = 0.0;
	self->anim_dy = MENU_ANIM_RISE;
	menu_anim_apply(self);
	wl_event_source_timer_update(self->anim_timer, 16);
}

static void
menu_render(GowlModuleMenu *self)
{
	GowlCompositor *comp = self->compositor;
	g_autoptr(GowlBarPanel) panel = NULL;
	GowlBarPanelRenderCtx ctx;
	PangoLayout *measure;
	cairo_surface_t *cs;
	cairo_t *cr;
	struct wlr_buffer *buf;
	struct wlr_scene_tree *overlay;
	gint mon_w, mon_h, width, max_h, radius, shadow;
	gint surf_w, surf_h;
	gint title_h, box_h, header_h, footer_h, list_h;
	gdouble scale;
	gboolean fresh_node = FALSE;

	if (!self->open || comp == NULL || self->theme == NULL)
		return;
	/*
	 * The output it was opened on can go while it is up -- unplugged,
	 * or its profile switched away.  The weak pointer says so, and a
	 * card with no screen to be on closes rather than drawing at
	 * coordinates that now belong to nothing.
	 */
	if (self->monitor == NULL || self->monitor->wlr_output == NULL) {
		menu_hide(self);
		return;
	}

	mon_w = self->monitor->w.width;
	mon_h = self->monitor->w.height;
	if (mon_w <= 0 || mon_h <= 0)
		return;

	width = self->want_width > 0
		? self->want_width
		: gowl_bar_theme_metric(self->theme,
		                        GOWL_BAR_METRIC_PANEL_WIDTH) + 120;
	if (width > mon_w - 40)
		width = mon_w - 40;
	if (width < 240)
		width = 240;

	max_h = self->want_height > 0 ? self->want_height : (mon_h * 7) / 10;
	if (max_h < 160)
		max_h = 160;
	if (max_h > mon_h - 40)
		max_h = mon_h - 40;

	menu_header_metrics(self, &title_h, &box_h, &header_h, &footer_h);

	panel = menu_build_panel(self);
	gowl_bar_panel_set_width(panel, width);

	/* Measure on a throwaway context: the surface cannot be sized
	   until the content has been, and the content cannot be measured
	   without a Pango context to measure it with. */
	{
		cairo_surface_t *tmp;
		cairo_t *tmp_cr;

		tmp = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
		tmp_cr = cairo_create(tmp);
		measure = pango_cairo_create_layout(tmp_cr);
		self->content_h = gowl_bar_panel_measure(panel, measure,
		                                         self->theme, width);
		g_object_unref(measure);
		cairo_destroy(tmp_cr);
		cairo_surface_destroy(tmp);
	}

	self->frame_w  = width;
	self->header_h = header_h;
	list_h = self->content_h;
	if (header_h + list_h + footer_h > max_h)
		list_h = max_h - header_h - footer_h;
	if (list_h < 40)
		list_h = 40;
	self->list_h  = list_h;
	self->frame_h = header_h + list_h + footer_h;

	if (self->scroll > self->content_h - self->list_h)
		self->scroll = self->content_h - self->list_h;
	if (self->scroll < 0)
		self->scroll = 0;

	shadow = gowl_bar_theme_metric(self->theme, GOWL_BAR_METRIC_SHADOW);
	radius = gowl_bar_theme_metric(self->theme, GOWL_BAR_METRIC_RADIUS);

	self->frame_x = shadow;
	self->frame_y = shadow;
	surf_w = self->frame_w + 2 * shadow;
	surf_h = self->frame_h + 2 * shadow;

	/* Centred on the output it opened on, which is where somebody who
	   just pressed a key is looking -- a little above the middle, as
	   every launcher sits, so a list that grows grows downward. */
	self->surf_x = self->monitor->w.x + (mon_w - surf_w) / 2;
	self->surf_y = self->monitor->w.y + (mon_h - surf_h) / 2;
	if (self->surf_y > self->monitor->w.y + mon_h / 8)
		self->surf_y -= (self->surf_y - self->monitor->w.y) / 5;

	scale = (self->monitor->wlr_output != NULL
	         && self->monitor->wlr_output->scale > 0.0f)
		? (gdouble)self->monitor->wlr_output->scale : 1.0;

	cs = cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
	                                (gint)ceil(surf_w * scale),
	                                (gint)ceil(surf_h * scale));
	if (cairo_surface_status(cs) != CAIRO_STATUS_SUCCESS) {
		cairo_surface_destroy(cs);
		return;
	}
	cr = cairo_create(cs);
	cairo_scale(cr, scale, scale);

	cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
	cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.0);
	cairo_paint(cr);
	cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

	gowl_bar_panel_draw_frame(cr, self->theme, self->frame_x, self->frame_y,
	                          self->frame_w, self->frame_h, NULL);

	if (self->hits == NULL) {
		self->hits = g_array_new(FALSE, FALSE, sizeof(GowlBarHitRect));
		g_array_set_clear_func(self->hits, gowl_bar_hit_rect_clear);
	}
	g_array_set_size(self->hits, 0);

	/* The header, then the rows in their own clip, then the footer. */
	cairo_save(cr);
	gowl_bar_cairo_rounded_rect(cr, self->frame_x, self->frame_y,
	                            self->frame_w, self->frame_h, radius);
	cairo_clip(cr);

	menu_draw_header(self, cr, self->frame_x, self->frame_y, self->frame_w,
	                 title_h, box_h);

	cairo_save(cr);
	cairo_rectangle(cr, self->frame_x, self->frame_y + header_h,
	                self->frame_w, list_h);
	cairo_clip(cr);
	cairo_translate(cr, self->frame_x, self->frame_y + header_h);
	{
		PangoLayout *layout = pango_cairo_create_layout(cr);

		gowl_bar_panel_render_ctx_init(&ctx, self->frame_w);
		ctx.max_height = list_h;
		ctx.scroll     = self->scroll;
		ctx.hits       = self->hits;
		/* The renderer wants item indexes; the cursor and the hover
		 * are rows.  The two maps built with the panel translate. */
		ctx.focus_item = menu_item_of_row(self, self->cursor);
		ctx.hover_item = menu_item_of_row(self, self->hover);

		gowl_bar_panel_render(panel, cr, layout, self->theme, &ctx);
		g_object_unref(layout);
	}
	cairo_restore(cr);

	/* Without an indicator a card taller than its cap looks like a card
	   that is simply missing its last rows. */
	if (self->content_h > list_h) {
		gdouble track_y, track_h, thumb_h, thumb_y;

		track_y = (gdouble)(self->frame_y + header_h) + 4.0;
		track_h = (gdouble)list_h - 8.0;
		thumb_h = track_h * (gdouble)list_h / (gdouble)self->content_h;
		if (thumb_h < 18.0)
			thumb_h = 18.0;
		thumb_y = track_y + (track_h - thumb_h) * (gdouble)self->scroll
		          / (gdouble)(self->content_h - list_h);

		gowl_bar_cairo_rounded_rect(cr,
			(gdouble)(self->frame_x + self->frame_w) - 6.0,
			thumb_y, 3.0, thumb_h, 1.5);
		gowl_bar_theme_cairo_set_alpha(self->theme, cr,
		                               GOWL_BAR_COLOR_ACCENT, 0.6);
		cairo_fill(cr);
	}

	menu_draw_footer(self, cr, self->frame_x,
	                 self->frame_y + header_h + list_h, self->frame_w,
	                 footer_h);
	cairo_restore(cr);

	cairo_destroy(cr);
	cairo_surface_flush(cs);

	buf = gowl_raw_buffer_create(cairo_image_surface_get_data(cs),
	                             cairo_image_surface_get_width(cs),
	                             cairo_image_surface_get_height(cs),
	                             cairo_image_surface_get_stride(cs));
	cairo_surface_destroy(cs);
	if (buf == NULL)
		return;

	overlay = gowl_compositor_get_scene_layer(comp, GOWL_SCENE_LAYER_OVERLAY);
	if (overlay == NULL) {
		/* No scene: the rows and the cursor are all still real, which
		 * is what a module test exercises.  Only the pixels are not. */
		wlr_buffer_drop(buf);
		return;
	}
	if (self->node == NULL) {
		self->node = wlr_scene_buffer_create(overlay, buf);
		if (self->node != NULL)
			self->node->point_accepts_input = menu_passthrough;
		fresh_node = TRUE;
	} else {
		wlr_scene_buffer_set_buffer(self->node, buf);
	}
	wlr_buffer_drop(buf);
	if (self->node == NULL)
		return;

	wlr_scene_buffer_set_dest_size(self->node, surf_w, surf_h);
	wlr_scene_node_raise_to_top(&self->node->node);
	if (fresh_node)
		menu_anim_start(self);
	menu_anim_apply(self);
}

/*
 * Scroll so the cursor is on screen.
 *
 * Read off the LAST render's hit rectangles rather than re-derived from
 * row heights: a row's height depends on whether it has a subtitle, and
 * arithmetic that has to agree with the renderer about that is
 * arithmetic that will stop agreeing with it.
 */
static void
menu_scroll_to_cursor(GowlModuleMenu *self)
{
	guint i;
	gint want;

	if (self->hits == NULL || self->cursor < 0)
		return;
	want = menu_item_of_row(self, self->cursor);
	for (i = 0; i < self->hits->len; i++) {
		GowlBarHitRect *r = &g_array_index(self->hits, GowlBarHitRect, i);

		if (r->item_index != want)
			continue;
		if (r->y < 0)
			self->scroll += r->y - 8;
		else if (r->y + r->height > self->list_h)
			self->scroll += (r->y + r->height) - self->list_h + 8;
		if (self->scroll < 0)
			self->scroll = 0;
		return;
	}
	/*
	 * The cursor moved to a row the last pass never drew, which is
	 * what happens walking into a long list: aim at it by proportion
	 * and let the next pass settle it.
	 */
	if (self->rows != NULL && self->rows->len > 0
	    && self->content_h > self->list_h) {
		self->scroll = (self->content_h - self->list_h)
		               * self->cursor / (gint)self->rows->len;
	}
}

static void
menu_move_cursor(GowlModuleMenu *self, gint delta)
{
	gint i;
	gint step = delta > 0 ? 1 : -1;
	gint n = delta > 0 ? delta : -delta;

	for (i = 0; i < n; i++) {
		gint next = menu_step(self, self->cursor, step);

		if (next < 0)
			break;
		self->cursor = next;
	}
	menu_scroll_to_cursor(self);
	menu_render(self);
}

/* ── Keyboard ────────────────────────────────────────────────────── */

static void
menu_set_filter(GowlModuleMenu *self, const gchar *text)
{
	g_autofree gchar *copy = g_strdup(text != NULL ? text : "");

	g_string_assign(self->filter, copy);
	menu_reload_rows(self);
	self->cursor = menu_step(self, -1, 1);
	menu_render(self);
}

/* Drop the last character of the filter. */
static void
menu_filter_backspace(GowlModuleMenu *self)
{
	gchar *last;

	if (self->filter->len == 0)
		return;
	last = g_utf8_find_prev_char(self->filter->str,
	                             self->filter->str + self->filter->len);
	g_string_truncate(self->filter,
		last != NULL ? (gsize)(last - self->filter->str) : 0);
	menu_set_filter(self, self->filter->str);
}

/* Drop the last word of the filter, the way Ctrl+w does at a prompt. */
static void
menu_filter_kill_word(GowlModuleMenu *self)
{
	gsize end = self->filter->len;

	while (end > 0 && self->filter->str[end - 1] == ' ')
		end--;
	while (end > 0 && self->filter->str[end - 1] != ' ')
		end--;
	g_string_truncate(self->filter, end);
	menu_set_filter(self, self->filter->str);
}

/* Back out one layer: the search first, then the level.  At the root
 * with nothing typed there is nothing to back out of, and the card
 * stays -- holding Backspace to clear a search used to run straight
 * through the empty box and close the menu. */
static void
menu_back(GowlModuleMenu *self)
{
	if (self->filter->len > 0) {
		menu_set_filter(self, "");
		return;
	}
	if (g_strcmp0(self->route, "root") != 0)
		menu_go_up(self);
}

/*
 * Every key while the card is up, and nothing gets past.
 *
 * Modules are offered a key only after the user's own binds have
 * declined it, so the key that opened the menu still reaches the bind
 * that opened it -- which is what makes the same key close it again.
 */
static gboolean
menu_handle_key(GowlKeybindHandler *handler, guint modifiers, guint keysym,
                gboolean pressed)
{
	GowlModuleMenu *self = GOWL_MODULE_MENU(handler);
	gchar utf8[8];
	gint len;

	if (!self->open)
		return FALSE;
	/* Releases are swallowed but do nothing: without this the release
	 * of the key that opened the card would be read as typing. */
	if (!pressed)
		return TRUE;

	switch (keysym) {
	case XKB_KEY_Escape:
		/* Escape backs out one layer of what you did: the filter
		 * first, and only then the menu. */
		if (self->filter->len > 0)
			menu_set_filter(self, "");
		else
			menu_hide(self);
		return TRUE;
	case XKB_KEY_BackSpace:
		if (self->filter->len > 0)
			menu_filter_backspace(self);
		else if (g_strcmp0(self->route, "root") != 0)
			menu_go_up(self);
		/* At the root with nothing typed: nothing to undo.  The
		 * key that closes is Escape, and a repeat that ran past the
		 * end of a search should land on an empty box, not on the
		 * desktop. */
		return TRUE;
	case XKB_KEY_Left:
		if (self->filter->len == 0 && g_strcmp0(self->route, "root") != 0)
			menu_go_up(self);
		return TRUE;
	case XKB_KEY_Up:
		menu_move_cursor(self, -1);
		return TRUE;
	case XKB_KEY_Down:
	case XKB_KEY_Tab:
		menu_move_cursor(self, 1);
		return TRUE;
	case XKB_KEY_ISO_Left_Tab:
		menu_move_cursor(self, -1);
		return TRUE;
	case XKB_KEY_Page_Up:
		menu_move_cursor(self, -MENU_PAGE);
		return TRUE;
	case XKB_KEY_Page_Down:
		menu_move_cursor(self, MENU_PAGE);
		return TRUE;
	case XKB_KEY_Home:
		self->cursor = menu_step(self, -1, 1);
		self->scroll = 0;
		menu_render(self);
		return TRUE;
	case XKB_KEY_End:
		self->cursor = menu_step(self,
			self->rows != NULL ? (gint)self->rows->len : 0, -1);
		menu_scroll_to_cursor(self);
		menu_render(self);
		return TRUE;
	case XKB_KEY_Return:
	case XKB_KEY_KP_Enter:
	case XKB_KEY_Right:
		menu_activate_index(self, self->cursor);
		return TRUE;
	default:
		break;
	}

	/*
	 * Alt+1 .. Alt+9 choose the Nth row outright.  Alt, not a bare
	 * digit: `2*21' is a sum to type.
	 */
	if ((modifiers & WLR_MODIFIER_ALT) != 0
	    && (modifiers & (WLR_MODIFIER_CTRL | WLR_MODIFIER_LOGO)) == 0
	    && keysym >= XKB_KEY_1 && keysym <= XKB_KEY_9) {
		gint want = (gint)(keysym - XKB_KEY_1);

		if (self->rows != NULL && want < (gint)self->rows->len) {
			self->cursor = want;
			menu_activate_index(self, want);
		}
		return TRUE;
	}

	/*
	 * Anything that produces a character extends the filter.  Taken
	 * from the keysym rather than from the raw code so a Dvorak or an
	 * AZERTY keyboard types what is printed on it, and so an accented
	 * character arrives as one character rather than as a byte.
	 *
	 * Ctrl and Alt are excluded: those are shortcuts somewhere, and a
	 * card that swallowed Ctrl+C into a search box would be eating
	 * keys people are pressing on purpose.
	 */
	if ((modifiers & (WLR_MODIFIER_CTRL | WLR_MODIFIER_ALT
	                  | WLR_MODIFIER_LOGO)) != 0) {
		/*
		 * Except the ones that navigate.
		 *
		 * The arrows already do all of this, and are not where the
		 * hands are.  Plain h/j/k/l cannot be it -- every letter
		 * goes into the search, which is the whole point of the
		 * card -- so the modifier is what makes room for them, and
		 * Ctrl is the one this session's other bindings leave free
		 * over a grabbed keyboard.  n/p, u, w and g are the prompt's
		 * own: an Emacs hand reaches for them without thinking.
		 *
		 * Left and right are ACROSS LEVELS rather than across
		 * characters: this is a tree, and moving in it is the only
		 * thing those two directions could sensibly mean.
		 */
		if ((modifiers & (WLR_MODIFIER_ALT | WLR_MODIFIER_LOGO)) == 0) {
			switch (keysym) {
			case XKB_KEY_h:
			case XKB_KEY_H:
				menu_back(self);
				return TRUE;
			case XKB_KEY_j:
			case XKB_KEY_J:
			case XKB_KEY_n:
			case XKB_KEY_N:
				menu_move_cursor(self, 1);
				return TRUE;
			case XKB_KEY_k:
			case XKB_KEY_K:
			case XKB_KEY_p:
			case XKB_KEY_P:
				menu_move_cursor(self, -1);
				return TRUE;
			case XKB_KEY_l:
			case XKB_KEY_L:
				menu_activate_index(self, self->cursor);
				return TRUE;
			case XKB_KEY_u:
			case XKB_KEY_U:
				menu_set_filter(self, "");
				return TRUE;
			case XKB_KEY_w:
			case XKB_KEY_W:
			case XKB_KEY_BackSpace:
				menu_filter_kill_word(self);
				return TRUE;
			case XKB_KEY_g:
			case XKB_KEY_G:
				menu_hide(self);
				return TRUE;
			default:
				break;
			}
		}
		return TRUE;
	}

	len = xkb_keysym_to_utf8(keysym, utf8, sizeof utf8);
	if (len > 1 && (guchar)utf8[0] >= 0x20 && utf8[0] != 0x7f) {
		/* A leading space is never a search; it is a slip. */
		if (self->filter->len == 0 && utf8[0] == ' ')
			return TRUE;
		if (self->filter->len < MENU_FILTER_MAX) {
			g_string_append(self->filter, utf8);
			menu_set_filter(self, self->filter->str);
		}
	}
	return TRUE;
}

/* ── Pointer ─────────────────────────────────────────────────────── */

/* Whether a layout-space point is on the card at all. */
static gboolean
menu_point_inside(GowlModuleMenu *self, gdouble lx, gdouble ly)
{
	gint x = (gint)lx - self->surf_x - self->frame_x;
	gint y = (gint)ly - self->surf_y - self->frame_y;

	return self->open && x >= 0 && y >= 0
	       && x < self->frame_w && y < self->frame_h;
}

/*
 * The row under a layout-space point, or -1.
 *
 * -1 means NO ROW, which is not the same as not on the card: the header,
 * the section titles and the gaps between rows are all part of the card
 * and produce no hit rectangle.  Treating the two the same is what makes
 * a click on the title close the menu.
 */
static gint
menu_row_at(GowlModuleMenu *self, gdouble lx, gdouble ly)
{
	gint x, y, hit;

	if (!self->open || self->hits == NULL)
		return -1;
	if (!menu_point_inside(self, lx, ly))
		return -1;
	x = (gint)lx - self->surf_x - self->frame_x;
	y = (gint)ly - self->surf_y - self->frame_y - self->header_h;
	if (y < 0 || y >= self->list_h)
		return -1;

	hit = gowl_bar_hit_find(self->hits, x, y);
	if (hit < 0)
		return -1;
	return menu_row_of_item(self,
		g_array_index(self->hits, GowlBarHitRect, hit).item_index);
}

static gboolean
menu_handle_motion(GowlMouseHandler *handler, gdouble x, gdouble y)
{
	GowlModuleMenu *self = GOWL_MODULE_MENU(handler);
	gint row;

	if (!self->open)
		return FALSE;

	row = menu_row_at(self, x, y);
	if (row != self->hover) {
		self->hover = row;
		menu_render(self);
	}
	/*
	 * Claimed even off the card.  Focus follows the mouse here, and a
	 * pointer crossing a window while the menu is up would move the
	 * focus out from under everything the menu is about to do.
	 */
	return TRUE;
}

static gboolean
menu_handle_button(GowlMouseHandler *handler, guint button, guint state,
                   guint modifiers)
{
	GowlModuleMenu *self = GOWL_MODULE_MENU(handler);
	gint row;

	if (!self->open)
		return FALSE;
	if (state == 0)
		return TRUE;               /* swallow the release too */

	if (self->compositor == NULL || self->compositor->wlr_cursor == NULL)
		return TRUE;

	/* The button event carries no position, so the pointer is read
	 * from the cursor -- layout coordinates, which is what the card
	 * was placed in. */
	/* A click outside the card closes it, which is what every menu
	 * everywhere does and the only discoverable way out for somebody
	 * who reached for the mouse.  A click ON the card but not on a row
	 * -- the title, the rule, the gap between two rows -- does
	 * nothing, because closing the menu is not what anybody means by
	 * clicking its header. */
	if (!menu_point_inside(self, self->compositor->wlr_cursor->x,
	                       self->compositor->wlr_cursor->y)) {
		menu_hide(self);
		return TRUE;
	}

	row = menu_row_at(self, self->compositor->wlr_cursor->x,
	                  self->compositor->wlr_cursor->y);
	if (row < 0)
		return TRUE;

	if (button == BTN_RIGHT) {
		menu_go_up(self);
		return TRUE;
	}
	if (button == BTN_LEFT) {
		self->cursor = row;
		menu_activate_index(self, row);
	}
	return TRUE;
}

static gboolean
menu_handle_axis(GowlMouseHandler *handler, guint axis, gdouble delta,
                 gint discrete, guint modifiers)
{
	GowlModuleMenu *self = GOWL_MODULE_MENU(handler);

	if (!self->open)
		return FALSE;
	if (self->content_h <= self->frame_h)
		return TRUE;

	self->scroll += (discrete != 0) ? discrete * 48 : (gint)(delta * 2.0);
	menu_render(self);
	return TRUE;
}

/* ── Commands ────────────────────────────────────────────────────── */

static gchar *
menu_list_reply(GowlModuleMenu *self, const gchar *route)
{
	g_autofree gchar *resolved = gowl_menu_resolve(self->menu, route);
	g_autoptr(GPtrArray) rows = NULL;
	GString *out;
	guint i;

	rows = gowl_menu_list(self->menu, self->compositor, resolved);
	out = g_string_new(NULL);
	for (i = 0; i < rows->len; i++) {
		GowlMenuRow *r = g_ptr_array_index(rows, i);

		/* Tab-separated, one row per line: the shape `cut' and
		 * `read' already understand, since a script reading this is
		 * a shell script. */
		g_string_append_printf(out, "%s\t%s\t%s\t%s\n",
			r->route, r->label != NULL ? r->label : "",
			r->submenu ? "menu" : "action",
			r->disabled ? "disabled" : (r->checked ? "checked" : "-"));
	}
	return g_string_free(out, FALSE);
}

static gchar *
menu_handle_command(GowlIpcHandler *handler, const gchar *command,
                    const gchar *args)
{
	GowlModuleMenu *self = GOWL_MODULE_MENU(handler);
	const gchar *route = args != NULL && *args != '\0' ? args : NULL;

	if (command == NULL || self->compositor == NULL)
		return NULL;

	if (g_strcmp0(command, "menu") == 0
	    || g_strcmp0(command, "menu-toggle") == 0) {
		/* Toggling to the SAME place closes; toggling to a different
		 * route moves there, which is what makes one key per route
		 * work without every one of them being a close. */
		if (self->open) {
			g_autofree gchar *want =
				gowl_menu_resolve(self->menu, route);

			if (route == NULL
			    || g_strcmp0(want, self->route) == 0) {
				menu_hide(self);
				return g_strdup("OK closed");
			}
		}
		menu_open_route(self, route);
		return g_strdup(self->open ? "OK open" : "OK");
	}
	if (g_strcmp0(command, "menu-open") == 0
	    || g_strcmp0(command, "menu-summon") == 0) {
		menu_open_route(self, route);
		return g_strdup(self->open ? "OK open" : "OK");
	}
	if (g_strcmp0(command, "menu-close") == 0) {
		menu_hide(self);
		return g_strdup("OK");
	}
	if (g_strcmp0(command, "menu-refresh") == 0) {
		g_autoptr(GError) error = NULL;

		if (!gowl_menu_load(self->menu, &error)) {
			return g_strdup_printf("ERROR %s",
				error != NULL ? error->message : "no menu found");
		}
		if (self->open) {
			/* The open route may have gone with the reload. */
			if (!gowl_menu_has_route(self->menu, self->route)) {
				g_free(self->route);
				self->route = g_strdup("root");
			}
			menu_reload_rows(self);
			self->cursor = menu_step(self, -1, 1);
			menu_render(self);
		}
		return g_strdup_printf("OK %u entries",
		                       gowl_menu_n_entries(self->menu));
	}
	if (g_strcmp0(command, "menu-list") == 0)
		return menu_list_reply(self, route);
	if (g_strcmp0(command, "menu-forget") == 0) {
		gowl_menu_forget_history(self->menu);
		if (self->open) {
			menu_reload_rows(self);
			self->cursor = menu_step(self, -1, 1);
			menu_render(self);
		}
		return g_strdup("OK forgotten");
	}

	return NULL;
}

/* ── Lifecycle ───────────────────────────────────────────────────── */

static void
menu_detach(GowlModuleMenu *self)
{
	menu_hide(self);

	/*
	 * The worker first, and JOINED.  It writes into the result queue
	 * and pokes an fd that the event source below is about to be
	 * removed from; letting it run past this point is a write into
	 * freed memory in the compositor process.
	 */
	if (self->worker != NULL) {
		g_async_queue_push(self->requests, g_strdup(""));
		g_thread_join(self->worker);
		self->worker = NULL;
	}
	if (self->wake_source != NULL) {
		wl_event_source_remove(self->wake_source);
		self->wake_source = NULL;
	}
	if (self->wake_fd >= 0) {
		close(self->wake_fd);
		self->wake_fd = -1;
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
menu_display_destroyed(struct wl_listener *listener, void *data)
{
	GowlModuleMenu *self = wl_container_of(listener, self, display_destroy);

	(void)data;
	menu_detach(self);
}

static void
menu_on_startup(GowlStartupHandler *handler, gpointer compositor)
{
	GowlModuleMenu *self = GOWL_MODULE_MENU(handler);
	g_autoptr(GError) error = NULL;

	if (self->compositor == compositor)
		return;
	menu_detach(self);

	self->compositor = compositor;
	g_object_add_weak_pointer(G_OBJECT(compositor),
	                          (gpointer *)&self->compositor);
	/* A compositor that has not been started yet has no display --
	 * which is every module test, and is not a reason to abort. */
	{
		struct wl_display *display =
			gowl_compositor_get_wl_display(compositor);

		if (display != NULL) {
			self->display_destroy.notify = menu_display_destroyed;
			wl_display_add_destroy_listener(display,
			                                &self->display_destroy);
		}
	}

	/*
	 * The icon worker, and the pipe it answers on.
	 *
	 * Both are optional: a compositor with no event loop is every
	 * module test, and the menu still works there -- rows simply keep
	 * their glyphs, because nothing ever answers.
	 */
	{
		struct wl_event_loop *loop =
			gowl_compositor_get_event_loop(compositor);

		self->wake_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
		if (self->wake_fd >= 0 && loop != NULL) {
			self->wake_source = wl_event_loop_add_fd(loop,
				self->wake_fd, WL_EVENT_READABLE,
				menu_icons_ready, self);
		}
		if (self->worker == NULL) {
			self->worker = g_thread_try_new("gowl-menu-icons",
				menu_icon_worker, self, NULL);
			if (self->worker == NULL) {
				g_warning("menu: no icon thread; rows will "
				          "show glyphs instead of icons");
			}
		}
	}

	/*
	 * The SHARED tree, not one of this module's own: cmacs reads the
	 * same object through its own bindings, and two copies of the menu
	 * are two copies that can disagree about what is in it.
	 */
	self->menu = g_object_ref(gowl_menu_get_default());
	if (gowl_menu_n_entries(self->menu) == 0)
		gowl_menu_load(self->menu, &error);

	/*
	 * Say which it was, loudly, once.
	 *
	 * A menu with no entries opens onto an empty card and is otherwise
	 * silent, so "the key does nothing" is the only symptom of a file
	 * that was simply not where it was looked for -- which is the
	 * first thing to check and the last thing anybody thinks of.  The
	 * error carries the paths that were tried; GOWL_DATADIR cannot be
	 * named from here, because a module built by its own Makefile
	 * loses the quoting on it (see this module's Makefile).
	 */
	if (gowl_menu_n_entries(self->menu) == 0) {
		g_warning("menu: no entries -- %s.  Set $GOWL_MENU_FILE to "
		          "point at a menu.yaml.",
		          error != NULL ? error->message : "nothing was loaded");
	} else {
		g_message("menu: %u entries from %s",
		          gowl_menu_n_entries(self->menu),
		          gowl_menu_get_source(self->menu));
	}
	menu_read_style(self);
}

static void
menu_on_shutdown(GowlShutdownHandler *handler, gpointer compositor)
{
	(void)compositor;
	menu_detach(GOWL_MODULE_MENU(handler));
}

static void menu_ipc_init(GowlIpcHandlerInterface *iface)
{
	iface->handle_command = menu_handle_command;
}

static void menu_key_init(GowlKeybindHandlerInterface *iface)
{
	iface->handle_key = menu_handle_key;
}

static void menu_mouse_init(GowlMouseHandlerInterface *iface)
{
	iface->handle_button = menu_handle_button;
	iface->handle_motion = menu_handle_motion;
	iface->handle_axis   = menu_handle_axis;
}

static void menu_startup_init(GowlStartupHandlerInterface *iface)
{
	iface->on_startup = menu_on_startup;
}

static void menu_shutdown_init(GowlShutdownHandlerInterface *iface)
{
	iface->on_shutdown = menu_on_shutdown;
}

static gboolean
menu_activate(GowlModule *module)
{
	(void)module;
	return TRUE;
}

static void
menu_deactivate(GowlModule *module)
{
	menu_detach(GOWL_MODULE_MENU(module));
}

static const gchar *
menu_name(GowlModule *module)
{
	(void)module;
	return "menu";
}

static const gchar *
menu_desc(GowlModule *module)
{
	(void)module;
	return "One list of everything the session can be told to do";
}

static void
gowl_module_menu_finalize(GObject *object)
{
	GowlModuleMenu *self = GOWL_MODULE_MENU(object);

	menu_detach(self);
	g_clear_object(&self->menu);
	g_clear_pointer(&self->theme, gowl_bar_theme_free);
	g_clear_pointer(&self->icons, g_hash_table_unref);
	g_clear_pointer(&self->asked, g_hash_table_unref);
	g_clear_pointer(&self->requests, g_async_queue_unref);
	g_clear_pointer(&self->results, g_async_queue_unref);
	g_clear_pointer(&self->hits, g_array_unref);
	g_clear_pointer(&self->item_rows, g_array_unref);
	g_clear_pointer(&self->row_items, g_array_unref);
	g_clear_pointer(&self->rows, g_ptr_array_unref);
	if (self->filter != NULL)
		g_string_free(self->filter, TRUE);
	g_free(self->route);
	G_OBJECT_CLASS(gowl_module_menu_parent_class)->finalize(object);
}

static void
gowl_module_menu_class_init(GowlModuleMenuClass *klass)
{
	GowlModuleClass *module = GOWL_MODULE_CLASS(klass);

	module->activate        = menu_activate;
	module->deactivate      = menu_deactivate;
	module->get_name        = menu_name;
	module->get_description = menu_desc;
	G_OBJECT_CLASS(klass)->finalize = gowl_module_menu_finalize;
}

static void
gowl_module_menu_init(GowlModuleMenu *self)
{
	wl_list_init(&self->display_destroy.link);
	self->filter   = g_string_new(NULL);
	self->icons    = gowl_bar_icon_cache_new();
	self->asked    = g_hash_table_new_full(g_str_hash, g_str_equal,
	                                       g_free, NULL);
	self->requests = g_async_queue_new_full(g_free);
	self->results  = g_async_queue_new_full(menu_icon_free);
	self->route    = g_strdup("root");
	self->icon_px  = 24;
	self->wake_fd  = -1;
	self->cursor = -1;
	self->hover  = -1;
	self->item_rows  = g_array_new(FALSE, FALSE, sizeof(gint));
	self->row_items  = g_array_new(FALSE, FALSE, sizeof(gint));
	self->anim_alpha = 1.0;
	self->animate    = TRUE;
	self->show_hints = TRUE;
	self->recent_max = 5;
}

G_MODULE_EXPORT GType
gowl_module_register(void)
{
	return gowl_module_menu_get_type();
}
