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
 * The StatusNotifierItem register.  See gowl-tray.h for what this is
 * and what it deliberately is not.
 *
 * THE PROTOCOL IS A MESS AND THIS FILE IS WHERE THAT IS ABSORBED.  It
 * was written by KDE, adopted by Ubuntu under a different name, and
 * implemented against whatever the author's own tray happened to
 * accept.  Four places where applications disagree, and what is done
 * about each:
 *
 *   THE INTERFACE HAS TWO NAMES.  `org.kde.StatusNotifierItem' and
 *   `org.freedesktop.StatusNotifierItem' are the same interface; which
 *   one an application publishes depends on which library it linked.
 *   Every read here tries the KDE name and falls back to the other.
 *
 *   REGISTRATION TAKES THREE SHAPES.  The argument to
 *   `RegisterStatusNotifierItem' is documented as a service name, but
 *   applications pass a bus name, an object path, or the two joined.
 *   All three are accepted, and a bare path is attributed to whoever
 *   sent the message --- which is also the only correct reading, since
 *   a path alone says nothing about whose it is.
 *
 *   PROPERTIES ARE RE-READ, NOT LISTENED FOR.  Almost nothing emits
 *   `PropertiesChanged'.  What applications do emit is `NewIcon',
 *   `NewStatus' and friends, which carry no payload at all, so each one
 *   is answered with a fresh `GetAll'.
 *
 *   AN APPLICATION MAY SIMPLY STOP ANSWERING.  Every call made here is
 *   on the bus thread with a short timeout, and never on the compositor
 *   thread, so a wedged tray application costs a tray icon rather than
 *   the desktop.
 */

#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "gowl-tray"

#include "tray/gowl-tray.h"

#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <sys/eventfd.h>

#include <gio/gio.h>

/*
 * How long an application gets to answer.
 *
 * Deliberately short.  This is a call into another process that may be
 * swapped out, wedged on the network or in the middle of its own
 * startup; the cost of giving up early is one icon that appears a
 * moment later when something else prompts a re-read, and the cost of
 * waiting is a bus thread that stops noticing anything else.
 */
#define GOWL_TRAY_CALL_TIMEOUT_MS (1500)

/* The KDE spelling first, because it is what nearly everything uses. */
#define SNI_IFACE_KDE  "org.kde.StatusNotifierItem"
#define SNI_IFACE_FDO  "org.freedesktop.StatusNotifierItem"
#define SNW_NAME       "org.kde.StatusNotifierWatcher"
#define SNW_PATH       "/StatusNotifierWatcher"
#define MENU_IFACE     "com.canonical.dbusmenu"

/*
 * The watcher, as applications see it.
 *
 * `RegisterStatusNotifierHost' and the host list are here because the
 * specification has them and applications check them: several will not
 * publish an item at all until somebody has said they are willing to
 * display one.
 */
static const gchar watcher_xml[] =
	"<node>"
	"  <interface name='org.kde.StatusNotifierWatcher'>"
	"    <method name='RegisterStatusNotifierItem'>"
	"      <arg type='s' name='service' direction='in'/>"
	"    </method>"
	"    <method name='RegisterStatusNotifierHost'>"
	"      <arg type='s' name='service' direction='in'/>"
	"    </method>"
	"    <property name='RegisteredStatusNotifierItems' type='as'"
	"              access='read'/>"
	"    <property name='IsStatusNotifierHostRegistered' type='b'"
	"              access='read'/>"
	"    <property name='ProtocolVersion' type='i' access='read'/>"
	"    <signal name='StatusNotifierItemRegistered'>"
	"      <arg type='s' name='service'/>"
	"    </signal>"
	"    <signal name='StatusNotifierItemUnregistered'>"
	"      <arg type='s' name='service'/>"
	"    </signal>"
	"    <signal name='StatusNotifierHostRegistered'/>"
	"    <signal name='StatusNotifierHostUnregistered'/>"
	"  </interface>"
	"</node>";

/* One item, bus-thread side: the published snapshot plus the
 * subscriptions that keep it current. */
typedef struct {
	GowlTrayItem      item;        /* the published half */
	guint             watch_id;    /* NameOwnerChanged for its bus name */
	guint             sig_id;      /* the item's own New* signals */
	guint             menu_sig_id; /* the menu's LayoutUpdated */
	GowlTrayMenuItem *menu;        /* last read layout, or NULL */
	gboolean          menu_wanted; /* a refresh is outstanding */
} TrayEntry;

struct _GowlTray {
	GObject parent_instance;

	/* The bus thread. */
	GThread         *thread;
	GMainContext    *ctx;
	GMainLoop       *loop;
	GDBusConnection *conn;         /* bus thread only */
	guint            watcher_name_id;
	guint            host_name_id;
	guint            reg_id;
	gboolean         stopping;

	/* Shared.  Everything under it is written by the bus thread and
	 * read by the compositor thread. */
	GMutex           lock;
	gboolean         serving;
	guint            serial;
	GHashTable      *entries;      /* key -> TrayEntry*, owned */
	GPtrArray       *order;        /* key strings, registration order */
	gboolean         host_registered;

	/* Bus thread -> compositor thread. */
	int                     wake_fd;
	struct wl_event_source *wake_source;
};

G_DEFINE_TYPE(GowlTray, gowl_tray, G_TYPE_OBJECT)

enum { SIGNAL_CHANGED, N_SIGNALS };
static guint signals[N_SIGNALS];

/* ── Boxed types ─────────────────────────────────────────────────── */

GowlTrayItem *
gowl_tray_item_copy(const GowlTrayItem *self)
{
	GowlTrayItem *copy;

	if (self == NULL)
		return NULL;
	copy = g_new0(GowlTrayItem, 1);
	copy->key                 = g_strdup(self->key);
	copy->service             = g_strdup(self->service);
	copy->path                = g_strdup(self->path);
	copy->id                  = g_strdup(self->id);
	copy->title               = g_strdup(self->title);
	copy->status              = g_strdup(self->status);
	copy->category            = g_strdup(self->category);
	copy->icon_name           = g_strdup(self->icon_name);
	copy->attention_icon_name = g_strdup(self->attention_icon_name);
	copy->icon_theme_path     = g_strdup(self->icon_theme_path);
	copy->tooltip             = g_strdup(self->tooltip);
	copy->menu_path           = g_strdup(self->menu_path);
	copy->item_is_menu        = self->item_is_menu;
	copy->pixmap_width        = self->pixmap_width;
	copy->pixmap_height       = self->pixmap_height;
	copy->serial              = self->serial;
	if (self->pixmap != NULL && self->pixmap_width > 0
	    && self->pixmap_height > 0) {
		gsize n = (gsize)self->pixmap_width * self->pixmap_height * 4;

		copy->pixmap = g_memdup2(self->pixmap, n);
	}
	return copy;
}

void
gowl_tray_item_free(GowlTrayItem *self)
{
	if (self == NULL)
		return;
	g_free(self->key);
	g_free(self->service);
	g_free(self->path);
	g_free(self->id);
	g_free(self->title);
	g_free(self->status);
	g_free(self->category);
	g_free(self->icon_name);
	g_free(self->attention_icon_name);
	g_free(self->icon_theme_path);
	g_free(self->tooltip);
	g_free(self->menu_path);
	g_free(self->pixmap);
	g_free(self);
}

G_DEFINE_BOXED_TYPE(GowlTrayItem, gowl_tray_item,
                    gowl_tray_item_copy, gowl_tray_item_free)

GowlTrayMenuItem *
gowl_tray_menu_item_copy(const GowlTrayMenuItem *self)
{
	GowlTrayMenuItem *copy;

	if (self == NULL)
		return NULL;
	copy = g_new0(GowlTrayMenuItem, 1);
	copy->id           = self->id;
	copy->label        = g_strdup(self->label);
	copy->icon_name    = g_strdup(self->icon_name);
	copy->is_separator = self->is_separator;
	copy->enabled      = self->enabled;
	copy->visible      = self->visible;
	copy->toggle_type  = g_strdup(self->toggle_type);
	copy->toggle_state = self->toggle_state;
	if (self->children != NULL) {
		guint i;

		copy->children = g_ptr_array_new_with_free_func(
			(GDestroyNotify)gowl_tray_menu_item_free);
		for (i = 0; i < self->children->len; i++) {
			g_ptr_array_add(copy->children, gowl_tray_menu_item_copy(
				g_ptr_array_index(self->children, i)));
		}
	}
	return copy;
}

void
gowl_tray_menu_item_free(GowlTrayMenuItem *self)
{
	if (self == NULL)
		return;
	g_free(self->label);
	g_free(self->icon_name);
	g_free(self->toggle_type);
	if (self->children != NULL)
		g_ptr_array_unref(self->children);
	g_free(self);
}

G_DEFINE_BOXED_TYPE(GowlTrayMenuItem, gowl_tray_menu_item,
                    gowl_tray_menu_item_copy, gowl_tray_menu_item_free)

cairo_surface_t *
gowl_tray_item_argb32(const GowlTrayItem *item)
{
	cairo_surface_t *surface;
	guint8 *dst;
	gint stride, x, y, w, h;

	if (item == NULL || item->pixmap == NULL
	    || item->pixmap_width <= 0 || item->pixmap_height <= 0)
		return NULL;

	w = item->pixmap_width;
	h = item->pixmap_height;
	surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
	if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
		cairo_surface_destroy(surface);
		return NULL;
	}
	dst = cairo_image_surface_get_data(surface);
	stride = cairo_image_surface_get_stride(surface);

	for (y = 0; y < h; y++) {
		for (x = 0; x < w; x++) {
			const guint8 *s = item->pixmap + ((gsize)y * w + x) * 4;
			guint32 *d = (guint32 *)(dst + (gsize)y * stride) + x;
			guint a = s[0], r = s[1], g = s[2], b = s[3];

			r = (r * a + 127) / 255;
			g = (g * a + 127) / 255;
			b = (b * a + 127) / 255;
			*d = ((guint32)a << 24) | ((guint32)r << 16)
			   | ((guint32)g << 8) | (guint32)b;
		}
	}
	cairo_surface_mark_dirty(surface);
	return surface;
}

static void
tray_entry_free(gpointer data)
{
	TrayEntry *e = data;

	if (e == NULL)
		return;
	g_free(e->item.key);
	g_free(e->item.service);
	g_free(e->item.path);
	g_free(e->item.id);
	g_free(e->item.title);
	g_free(e->item.status);
	g_free(e->item.category);
	g_free(e->item.icon_name);
	g_free(e->item.attention_icon_name);
	g_free(e->item.icon_theme_path);
	g_free(e->item.tooltip);
	g_free(e->item.menu_path);
	g_free(e->item.pixmap);
	if (e->menu != NULL)
		gowl_tray_menu_item_free(e->menu);
	g_free(e);
}

/* ── Waking the compositor ───────────────────────────────────────── */

/*
 * Bumped on the bus thread under the lock, and the compositor thread is
 * poked through an eventfd so it re-reads at a moment of its choosing.
 * Emitting a GObject signal from the bus thread instead would run every
 * handler -- and any Lisp behind them -- on the wrong thread.
 */
static void
tray_changed_locked(GowlTray *self)
{
	guint64 one = 1;
	gssize  r;

	self->serial++;
	if (self->wake_fd < 0)
		return;
	do {
		r = write(self->wake_fd, &one, sizeof one);
	} while (r < 0 && errno == EINTR);
}

static int
on_wake(int fd, uint32_t mask, void *data)
{
	GowlTray *self = data;
	guint64   drained;
	gssize    r;

	(void)mask;
	do {
		r = read(fd, &drained, sizeof drained);
	} while (r < 0 && errno == EINTR);

	g_signal_emit(self, signals[SIGNAL_CHANGED], 0);
	return 0;
}

/* ── Reading an item ─────────────────────────────────────────────── */

static gchar *
dup_string_prop(GVariant *props, const gchar *name)
{
	g_autoptr(GVariant) v = NULL;

	if (props == NULL)
		return NULL;
	v = g_variant_lookup_value(props, name, G_VARIANT_TYPE_STRING);
	if (v == NULL)
		return NULL;
	return g_variant_dup_string(v, NULL);
}

static gchar *
dup_objpath_prop(GVariant *props, const gchar *name)
{
	g_autoptr(GVariant) v = NULL;

	if (props == NULL)
		return NULL;
	/* Menu is an object path, but plenty of applications publish it as
	 * a plain string; take either. */
	v = g_variant_lookup_value(props, name, G_VARIANT_TYPE_OBJECT_PATH);
	if (v == NULL)
		v = g_variant_lookup_value(props, name, G_VARIANT_TYPE_STRING);
	if (v == NULL)
		return NULL;
	return g_variant_dup_string(v, NULL);
}

/*
 * The tooltip is `(sa(iiay)ss)': an icon name, icon data, a title and a
 * body.  Only the title is wanted, and an application that publishes a
 * bare string instead -- several do -- is taken at its word.
 */
static gchar *
dup_tooltip(GVariant *props)
{
	g_autoptr(GVariant) v = NULL;
	g_autoptr(GVariant) title = NULL;

	if (props == NULL)
		return NULL;
	v = g_variant_lookup_value(props, "ToolTip", NULL);
	if (v == NULL)
		return NULL;
	if (g_variant_is_of_type(v, G_VARIANT_TYPE_STRING))
		return g_variant_dup_string(v, NULL);
	if (!g_variant_is_of_type(v, G_VARIANT_TYPE("(sa(iiay)ss)")))
		return NULL;
	title = g_variant_get_child_value(v, 2);
	if (title == NULL || !g_variant_is_of_type(title, G_VARIANT_TYPE_STRING))
		return NULL;
	return g_variant_dup_string(title, NULL);
}

/*
 * Pick one of the offered pixmaps.
 *
 * `IconPixmap' is `a(iiay)' -- any number of sizes, in no promised
 * order, and applications offer anything from a single 16x16 to a set
 * running to 512.  The smallest that is at least 24 pixels is the one
 * taken, falling back to the largest on offer: a bar icon is around
 * twenty pixels, so that is the smallest source that will not be
 * scaled UP, and hauling a 512 through a scaler every repaint to draw
 * it at 22 is a waste of a cache line and a lot of filtering.
 */
static void
take_pixmap(GVariant *props, guint8 **out, gint *out_w, gint *out_h)
{
	g_autoptr(GVariant) v = NULL;
	GVariantIter iter;
	GVariant *child;
	gint best_w = 0, best_h = 0;
	GVariant *best = NULL;

	*out = NULL;
	*out_w = 0;
	*out_h = 0;
	if (props == NULL)
		return;
	v = g_variant_lookup_value(props, "IconPixmap", G_VARIANT_TYPE("a(iiay)"));
	if (v == NULL)
		return;

	g_variant_iter_init(&iter, v);
	while ((child = g_variant_iter_next_value(&iter)) != NULL) {
		gint w = 0, h = 0;
		g_autoptr(GVariant) bytes = NULL;
		gboolean better;

		g_variant_get_child(child, 0, "i", &w);
		g_variant_get_child(child, 1, "i", &h);
		bytes = g_variant_get_child_value(child, 2);
		if (w <= 0 || h <= 0
		    || g_variant_n_children(bytes) < (gsize)w * h * 4) {
			g_variant_unref(child);
			continue;
		}

		if (best == NULL) {
			better = TRUE;
		} else if (best_w < 24) {
			/* Anything at least as big as the target beats one
			 * that is too small, and so does merely being bigger. */
			better = w > best_w;
		} else {
			better = w >= 24 && w < best_w;
		}
		if (better) {
			g_clear_pointer(&best, g_variant_unref);
			best = g_variant_ref(child);
			best_w = w;
			best_h = h;
		}
		g_variant_unref(child);
	}
	if (best == NULL)
		return;

	{
		g_autoptr(GVariant) bytes = g_variant_get_child_value(best, 2);
		gsize n = 0;
		const guint8 *data = g_variant_get_fixed_array(bytes, &n, 1);

		if (data != NULL && n >= (gsize)best_w * best_h * 4) {
			*out = g_memdup2(data, (gsize)best_w * best_h * 4);
			*out_w = best_w;
			*out_h = best_h;
		}
	}
	g_variant_unref(best);
}

/* Every property of one item, from whichever spelling of the interface
 * the application answers to.  Bus thread. */
static GVariant *
item_get_all(GowlTray *self, const gchar *service, const gchar *path)
{
	static const gchar *const ifaces[] = { SNI_IFACE_KDE, SNI_IFACE_FDO };
	guint i;

	for (i = 0; i < G_N_ELEMENTS(ifaces); i++) {
		g_autoptr(GVariant) reply = NULL;
		g_autoptr(GError) error = NULL;

		reply = g_dbus_connection_call_sync(self->conn, service, path,
			"org.freedesktop.DBus.Properties", "GetAll",
			g_variant_new("(s)", ifaces[i]), G_VARIANT_TYPE("(a{sv})"),
			G_DBUS_CALL_FLAGS_NONE, GOWL_TRAY_CALL_TIMEOUT_MS,
			NULL, &error);
		if (reply != NULL)
			return g_variant_get_child_value(reply, 0);
	}
	return NULL;
}

/* Fill an entry's published half from a GetAll reply.  Bus thread,
 * caller holds the lock. */
static void
entry_apply_props(TrayEntry *e, GVariant *props)
{
	g_free(e->item.id);
	g_free(e->item.title);
	g_free(e->item.status);
	g_free(e->item.category);
	g_free(e->item.icon_name);
	g_free(e->item.attention_icon_name);
	g_free(e->item.icon_theme_path);
	g_free(e->item.tooltip);
	g_free(e->item.menu_path);
	g_free(e->item.pixmap);

	e->item.id                 = dup_string_prop(props, "Id");
	e->item.title              = dup_string_prop(props, "Title");
	e->item.status             = dup_string_prop(props, "Status");
	e->item.category           = dup_string_prop(props, "Category");
	e->item.icon_name          = dup_string_prop(props, "IconName");
	e->item.attention_icon_name = dup_string_prop(props, "AttentionIconName");
	e->item.icon_theme_path    = dup_string_prop(props, "IconThemePath");
	e->item.tooltip            = dup_tooltip(props);
	e->item.menu_path          = dup_objpath_prop(props, "Menu");
	e->item.item_is_menu       = FALSE;
	take_pixmap(props, &e->item.pixmap,
	            &e->item.pixmap_width, &e->item.pixmap_height);

	if (props != NULL) {
		g_autoptr(GVariant) v =
			g_variant_lookup_value(props, "ItemIsMenu",
			                       G_VARIANT_TYPE_BOOLEAN);

		if (v != NULL)
			e->item.item_is_menu = g_variant_get_boolean(v);
	}

	/* An item with a menu path of "/" has none: the specification's own
	 * way of saying so, and several applications use it. */
	if (g_strcmp0(e->item.menu_path, "/") == 0)
		g_clear_pointer(&e->item.menu_path, g_free);

	e->item.serial++;
}

/* ── The register ────────────────────────────────────────────────── */

static void item_signal_cb (GDBusConnection *conn, const gchar *sender,
                            const gchar *path, const gchar *iface,
                            const gchar *signal, GVariant *params,
                            gpointer data);
static void menu_signal_cb (GDBusConnection *conn, const gchar *sender,
                            const gchar *path, const gchar *iface,
                            const gchar *signal, GVariant *params,
                            gpointer data);
static void item_vanished_cb (GDBusConnection *conn, const gchar *name,
                              gpointer data);
static void tray_forget (GowlTray *self, const gchar *key);

typedef struct {
	GowlTray *tray;
	gchar    *key;
} EntryRef;

static void
entry_ref_free(gpointer data)
{
	EntryRef *r = data;

	g_free(r->key);
	g_free(r);
}

static EntryRef *
entry_ref_new(GowlTray *self, const gchar *key)
{
	EntryRef *r = g_new0(EntryRef, 1);

	r->tray = self;
	r->key  = g_strdup(key);
	return r;
}

/*
 * Split what an application passed to `RegisterStatusNotifierItem'.
 *
 * Three shapes are in the wild and all three are accepted:
 *
 *   "org.example.App"                a bus name, path is the default
 *   "/StatusNotifierItem"            a path, on the SENDER's bus name
 *   "org.example.App/Status..."      both, joined
 *
 * @sender is who sent the message, which is the only thing that can
 * attribute a bare path -- and is also the safe reading for the other
 * two, but the specification allows an application to register on
 * behalf of another name, so a name given explicitly is honoured.
 */
static void
split_service(const gchar *service, const gchar *sender,
              gchar **out_bus, gchar **out_path)
{
	const gchar *slash;

	*out_bus = NULL;
	*out_path = NULL;
	if (service == NULL || *service == '\0') {
		*out_bus = g_strdup(sender);
		*out_path = g_strdup("/StatusNotifierItem");
		return;
	}
	if (service[0] == '/') {
		*out_bus = g_strdup(sender);
		*out_path = g_strdup(service);
		return;
	}
	slash = strchr(service, '/');
	if (slash == NULL) {
		*out_bus = g_strdup(service);
		*out_path = g_strdup("/StatusNotifierItem");
		return;
	}
	*out_bus = g_strndup(service, (gsize)(slash - service));
	*out_path = g_strdup(slash);
}

/* Bus thread. */
static void
tray_register_item(GowlTray *self, const gchar *service, const gchar *sender)
{
	g_autofree gchar *bus = NULL;
	g_autofree gchar *path = NULL;
	g_autofree gchar *key = NULL;
	g_autoptr(GVariant) props = NULL;
	TrayEntry *e;

	split_service(service, sender, &bus, &path);
	if (bus == NULL || path == NULL)
		return;
	key = g_strconcat(bus, path, NULL);

	g_mutex_lock(&self->lock);
	if (g_hash_table_contains(self->entries, key)) {
		/* A re-registration is a refresh, not a duplicate: several
		 * applications re-register after their own restart. */
		g_mutex_unlock(&self->lock);
		props = item_get_all(self, bus, path);
		g_mutex_lock(&self->lock);
		e = g_hash_table_lookup(self->entries, key);
		if (e != NULL && props != NULL)
			entry_apply_props(e, props);
		tray_changed_locked(self);
		g_mutex_unlock(&self->lock);
		return;
	}
	g_mutex_unlock(&self->lock);

	/* Read before publishing, so the item never appears blank. */
	props = item_get_all(self, bus, path);

	e = g_new0(TrayEntry, 1);
	e->item.key     = g_strdup(key);
	e->item.service = g_strdup(bus);
	e->item.path    = g_strdup(path);
	if (props != NULL)
		entry_apply_props(e, props);

	e->watch_id = g_bus_watch_name_on_connection(self->conn, bus,
		G_BUS_NAME_WATCHER_FLAGS_NONE, NULL, item_vanished_cb,
		entry_ref_new(self, key), entry_ref_free);

	/*
	 * One subscription for every signal the item emits, matched on the
	 * sender and path rather than the interface: an application that
	 * publishes the freedesktop spelling emits the freedesktop
	 * spelling, and matching on one of them loses half the updates.
	 */
	e->sig_id = g_dbus_connection_signal_subscribe(self->conn, bus,
		NULL, NULL, path, NULL, G_DBUS_SIGNAL_FLAGS_NONE,
		item_signal_cb, entry_ref_new(self, key), entry_ref_free);

	if (e->item.menu_path != NULL) {
		e->menu_sig_id = g_dbus_connection_signal_subscribe(self->conn,
			bus, MENU_IFACE, NULL, e->item.menu_path, NULL,
			G_DBUS_SIGNAL_FLAGS_NONE, menu_signal_cb,
			entry_ref_new(self, key), entry_ref_free);
	}

	g_mutex_lock(&self->lock);
	g_hash_table_insert(self->entries, g_strdup(key), e);
	g_ptr_array_add(self->order, g_strdup(key));
	tray_changed_locked(self);
	g_mutex_unlock(&self->lock);

	g_dbus_connection_emit_signal(self->conn, NULL, SNW_PATH, SNW_NAME,
		"StatusNotifierItemRegistered", g_variant_new("(s)", key), NULL);
	g_info("tray: %s registered (%s)", e->item.id != NULL ? e->item.id : key,
	       key);
}

/* Bus thread. */
static void
tray_forget(GowlTray *self, const gchar *key)
{
	TrayEntry *e;
	guint      watch_id = 0, sig_id = 0, menu_sig_id = 0;
	guint      i;

	g_mutex_lock(&self->lock);
	e = g_hash_table_lookup(self->entries, key);
	if (e == NULL) {
		g_mutex_unlock(&self->lock);
		return;
	}
	watch_id = e->watch_id;
	sig_id = e->sig_id;
	menu_sig_id = e->menu_sig_id;
	e->watch_id = e->sig_id = e->menu_sig_id = 0;

	for (i = 0; i < self->order->len; i++) {
		if (g_strcmp0(g_ptr_array_index(self->order, i), key) == 0) {
			g_ptr_array_remove_index(self->order, i);
			break;
		}
	}
	g_hash_table_remove(self->entries, key);
	tray_changed_locked(self);
	g_mutex_unlock(&self->lock);

	/* Outside the lock: unsubscribing waits for any handler already
	 * running, and that handler wants the lock. */
	if (sig_id != 0)
		g_dbus_connection_signal_unsubscribe(self->conn, sig_id);
	if (menu_sig_id != 0)
		g_dbus_connection_signal_unsubscribe(self->conn, menu_sig_id);
	if (watch_id != 0)
		g_bus_unwatch_name(watch_id);

	g_dbus_connection_emit_signal(self->conn, NULL, SNW_PATH, SNW_NAME,
		"StatusNotifierItemUnregistered", g_variant_new("(s)", key), NULL);
	g_info("tray: %s went away", key);
}

static void
item_vanished_cb(GDBusConnection *conn, const gchar *name, gpointer data)
{
	EntryRef *r = data;

	(void)conn;
	(void)name;
	tray_forget(r->tray, r->key);
}

/* Bus thread.  Any of the item's New* signals: re-read everything. */
static void
item_signal_cb(GDBusConnection *conn, const gchar *sender, const gchar *path,
               const gchar *iface, const gchar *signal, GVariant *params,
               gpointer data)
{
	EntryRef *r = data;
	GowlTray *self = r->tray;
	g_autoptr(GVariant) props = NULL;
	g_autofree gchar *bus = NULL;
	g_autofree gchar *obj = NULL;
	TrayEntry *e;

	(void)conn;
	(void)sender;
	(void)params;

	/* Only the item's own news.  The subscription is deliberately wide
	 * -- see where it is made -- so it also catches the Properties and
	 * Introspectable traffic on the same path. */
	if (signal == NULL || !g_str_has_prefix(signal, "New"))
		return;
	if (iface != NULL && !g_str_has_prefix(iface, "org.kde.StatusNotifier")
	    && !g_str_has_prefix(iface, "org.freedesktop.StatusNotifier"))
		return;

	g_mutex_lock(&self->lock);
	e = g_hash_table_lookup(self->entries, r->key);
	if (e == NULL) {
		g_mutex_unlock(&self->lock);
		return;
	}
	bus = g_strdup(e->item.service);
	obj = g_strdup(e->item.path);
	g_mutex_unlock(&self->lock);

	(void)path;
	props = item_get_all(self, bus, obj);
	if (props == NULL)
		return;

	g_mutex_lock(&self->lock);
	e = g_hash_table_lookup(self->entries, r->key);
	if (e != NULL) {
		entry_apply_props(e, props);
		tray_changed_locked(self);
	}
	g_mutex_unlock(&self->lock);
}

/* ── The menu ────────────────────────────────────────────────────── */

/*
 * dbusmenu's layout is `(ia{sv}av)' all the way down: an id, a bag of
 * properties and a list of children, each child a variant wrapping
 * another of the same.  Everything about a row is in the property bag
 * and every property is optional, so every default here is the one the
 * specification gives for an absent key.
 */
static GowlTrayMenuItem *
menu_parse(GVariant *node)
{
	GowlTrayMenuItem *item;
	g_autoptr(GVariant) props = NULL;
	g_autoptr(GVariant) children = NULL;
	g_autofree gchar *type = NULL;
	gchar *label;
	GVariantIter iter;
	GVariant *child;

	if (node == NULL
	    || !g_variant_is_of_type(node, G_VARIANT_TYPE("(ia{sv}av)")))
		return NULL;

	item = g_new0(GowlTrayMenuItem, 1);
	item->enabled      = TRUE;
	item->visible      = TRUE;
	item->toggle_state = -1;

	g_variant_get_child(node, 0, "i", &item->id);
	props = g_variant_get_child_value(node, 1);

	type = dup_string_prop(props, "type");
	item->is_separator = g_strcmp0(type, "separator") == 0;

	label = dup_string_prop(props, "label");
	if (label != NULL) {
		/*
		 * Strip the mnemonic underscores.  dbusmenu carries GTK's
		 * "_File" convention and there is no keyboard mnemonic to
		 * attach it to here, so an item would otherwise read "_Quit".
		 * A doubled underscore is a literal one.
		 */
		GString *out = g_string_new(NULL);
		const gchar *p;

		for (p = label; *p != '\0'; p++) {
			if (*p != '_') {
				g_string_append_c(out, *p);
			} else if (p[1] == '_') {
				g_string_append_c(out, '_');
				p++;
			}
		}
		g_free(label);
		item->label = g_string_free(out, FALSE);
	}

	item->icon_name   = dup_string_prop(props, "icon-name");
	item->toggle_type = dup_string_prop(props, "toggle-type");
	if (item->toggle_type != NULL && item->toggle_type[0] == '\0')
		g_clear_pointer(&item->toggle_type, g_free);

	{
		g_autoptr(GVariant) v = NULL;

		v = g_variant_lookup_value(props, "enabled",
		                           G_VARIANT_TYPE_BOOLEAN);
		if (v != NULL)
			item->enabled = g_variant_get_boolean(v);
	}
	{
		g_autoptr(GVariant) v = NULL;

		v = g_variant_lookup_value(props, "visible",
		                           G_VARIANT_TYPE_BOOLEAN);
		if (v != NULL)
			item->visible = g_variant_get_boolean(v);
	}
	{
		g_autoptr(GVariant) v = NULL;

		v = g_variant_lookup_value(props, "toggle-state",
		                           G_VARIANT_TYPE_INT32);
		if (v != NULL)
			item->toggle_state = g_variant_get_int32(v);
	}

	children = g_variant_get_child_value(node, 2);
	g_variant_iter_init(&iter, children);
	while ((child = g_variant_iter_next_value(&iter)) != NULL) {
		g_autoptr(GVariant) inner = g_variant_get_variant(child);
		GowlTrayMenuItem *sub = menu_parse(inner);

		if (sub != NULL) {
			if (item->children == NULL) {
				item->children = g_ptr_array_new_with_free_func(
					(GDestroyNotify)gowl_tray_menu_item_free);
			}
			g_ptr_array_add(item->children, sub);
		}
		g_variant_unref(child);
	}
	return item;
}

/* Bus thread.  Asks permission, then reads the whole tree. */
static void
tray_read_menu(GowlTray *self, const gchar *key)
{
	g_autofree gchar *bus = NULL;
	g_autofree gchar *menu_path = NULL;
	g_autoptr(GVariant) reply = NULL;
	g_autoptr(GVariant) layout = NULL;
	g_autoptr(GError) error = NULL;
	GowlTrayMenuItem *root;
	TrayEntry *e;

	g_mutex_lock(&self->lock);
	e = g_hash_table_lookup(self->entries, key);
	if (e == NULL || e->item.menu_path == NULL) {
		g_mutex_unlock(&self->lock);
		return;
	}
	bus = g_strdup(e->item.service);
	menu_path = g_strdup(e->item.menu_path);
	g_mutex_unlock(&self->lock);

	/*
	 * AboutToShow first, and its answer is ignored on purpose.  It is
	 * how an application that builds its menu on demand gets told to
	 * build it -- the Proton bridge's is empty until this lands -- and
	 * an application that has not implemented it returns an error,
	 * which is not a reason to skip the read that follows.
	 */
	g_dbus_connection_call_sync(self->conn, bus, menu_path, MENU_IFACE,
		"AboutToShow", g_variant_new("(i)", 0), NULL,
		G_DBUS_CALL_FLAGS_NONE, GOWL_TRAY_CALL_TIMEOUT_MS, NULL, NULL);

	/* Depth -1 is the whole tree.  Asking for one level and walking it
	 * would be a round trip per submenu, each able to time out. */
	reply = g_dbus_connection_call_sync(self->conn, bus, menu_path,
		MENU_IFACE, "GetLayout",
		g_variant_new("(iias)", 0, -1, NULL),
		G_VARIANT_TYPE("(u(ia{sv}av))"), G_DBUS_CALL_FLAGS_NONE,
		GOWL_TRAY_CALL_TIMEOUT_MS, NULL, &error);
	if (reply == NULL) {
		g_debug("tray: no menu from %s: %s", key,
		        error != NULL ? error->message : "?");
		return;
	}

	layout = g_variant_get_child_value(reply, 1);
	root = menu_parse(layout);
	if (root == NULL)
		return;

	g_mutex_lock(&self->lock);
	e = g_hash_table_lookup(self->entries, key);
	if (e != NULL) {
		if (e->menu != NULL)
			gowl_tray_menu_item_free(e->menu);
		e->menu = root;
		e->menu_wanted = FALSE;
		tray_changed_locked(self);
	} else {
		gowl_tray_menu_item_free(root);
	}
	g_mutex_unlock(&self->lock);
}

/* The application says its menu changed. */
static void
menu_signal_cb(GDBusConnection *conn, const gchar *sender, const gchar *path,
               const gchar *iface, const gchar *signal, GVariant *params,
               gpointer data)
{
	EntryRef *r = data;

	(void)conn; (void)sender; (void)path; (void)iface; (void)params;

	if (g_strcmp0(signal, "LayoutUpdated") != 0
	    && g_strcmp0(signal, "ItemsPropertiesUpdated") != 0)
		return;
	tray_read_menu(r->tray, r->key);
}

/* ── The watcher object ──────────────────────────────────────────── */

static void
watcher_method(GDBusConnection *conn, const gchar *sender,
               const gchar *path, const gchar *iface, const gchar *method,
               GVariant *params, GDBusMethodInvocation *inv, gpointer data)
{
	GowlTray *self = data;

	(void)conn; (void)path; (void)iface;

	if (g_strcmp0(method, "RegisterStatusNotifierItem") == 0) {
		const gchar *service = NULL;

		g_variant_get(params, "(&s)", &service);
		/* Answer first.  Reading the item's properties is a call back
		 * into the application that is making this one, and an
		 * application that waits for the reply before answering its own
		 * would deadlock for the length of the timeout. */
		g_dbus_method_invocation_return_value(inv, NULL);
		tray_register_item(self, service, sender);
		return;
	}
	if (g_strcmp0(method, "RegisterStatusNotifierHost") == 0) {
		g_dbus_method_invocation_return_value(inv, NULL);
		g_mutex_lock(&self->lock);
		self->host_registered = TRUE;
		g_mutex_unlock(&self->lock);
		g_dbus_connection_emit_signal(self->conn, NULL, SNW_PATH,
			SNW_NAME, "StatusNotifierHostRegistered", NULL, NULL);
		return;
	}
	g_dbus_method_invocation_return_error(inv, G_DBUS_ERROR,
		G_DBUS_ERROR_UNKNOWN_METHOD, "no such method: %s", method);
}

static GVariant *
watcher_get_property(GDBusConnection *conn, const gchar *sender,
                     const gchar *path, const gchar *iface,
                     const gchar *prop, GError **error, gpointer data)
{
	GowlTray *self = data;

	(void)conn; (void)sender; (void)path; (void)iface;

	if (g_strcmp0(prop, "ProtocolVersion") == 0)
		return g_variant_new_int32(0);
	if (g_strcmp0(prop, "IsStatusNotifierHostRegistered") == 0) {
		/*
		 * Always true while this is serving.  gowl IS the host -- the
		 * bar draws the items -- and an application that asks is
		 * deciding whether to publish an item at all.
		 */
		return g_variant_new_boolean(TRUE);
	}
	if (g_strcmp0(prop, "RegisteredStatusNotifierItems") == 0) {
		GVariantBuilder b;
		guint i;

		g_variant_builder_init(&b, G_VARIANT_TYPE("as"));
		g_mutex_lock(&self->lock);
		for (i = 0; i < self->order->len; i++) {
			g_variant_builder_add(&b, "s",
				(const gchar *)g_ptr_array_index(self->order, i));
		}
		g_mutex_unlock(&self->lock);
		return g_variant_builder_end(&b);
	}
	g_set_error(error, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_PROPERTY,
	            "no such property: %s", prop);
	return NULL;
}

static const GDBusInterfaceVTable watcher_vtable = {
	watcher_method, watcher_get_property, NULL, { 0 }
};

static void
on_watcher_name_acquired(GDBusConnection *conn, const gchar *name,
                         gpointer data)
{
	GowlTray *self = data;

	(void)conn;
	g_mutex_lock(&self->lock);
	self->serving = TRUE;
	tray_changed_locked(self);
	g_mutex_unlock(&self->lock);
	g_message("tray: serving %s", name);
}

static void
on_watcher_name_lost(GDBusConnection *conn, const gchar *name, gpointer data)
{
	GowlTray *self = data;
	gboolean  was;

	(void)conn;
	g_mutex_lock(&self->lock);
	was = self->serving;
	self->serving = FALSE;
	tray_changed_locked(self);
	g_mutex_unlock(&self->lock);

	if (was) {
		g_warning("tray: lost %s to another tray", name);
	} else {
		/*
		 * Somebody else had it first.  Not an error and not worth a
		 * warning: a GNOME shell extension, a second compositor or a
		 * cmacs started before this one is a perfectly ordinary thing
		 * to find, and two watchers on one bus is worse than one.
		 */
		g_message("tray: %s is already served; standing down", name);
	}
}

/* ── The bus thread ──────────────────────────────────────────────── */

static gpointer
bus_thread(gpointer data)
{
	GowlTray *self = data;
	g_autoptr(GError) error = NULL;
	GDBusNodeInfo *info;

	g_main_context_push_thread_default(self->ctx);

	self->conn = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &error);
	if (self->conn == NULL) {
		g_warning("tray: no session bus: %s", error->message);
		g_main_context_pop_thread_default(self->ctx);
		return NULL;
	}

	info = g_dbus_node_info_new_for_xml(watcher_xml, NULL);
	self->reg_id = g_dbus_connection_register_object(self->conn, SNW_PATH,
		info->interfaces[0], &watcher_vtable, self, NULL, &error);
	g_dbus_node_info_unref(info);
	if (self->reg_id == 0) {
		g_warning("tray: could not publish the watcher: %s",
		          error->message);
	} else {
		/*
		 * NONE: never TAKE the name from a tray that has it.
		 *
		 * It does queue --- DBUS_NAME_FLAG_DO_NOT_QUEUE is the opt-in
		 * and NONE is not it --- and that is wanted rather than
		 * tolerated.  Logging out and back in is a race with the
		 * outgoing session's compositor, which still owns the name
		 * when the incoming one asks; queueing is what turns that into
		 * a tray half a second late instead of no tray until the next
		 * reboot.  on_watcher_name_acquired() is what makes `serving'
		 * follow the handover, and tests/test-tray.c asserts the whole
		 * sequence against a bus of its own.
		 *
		 * The cost is real but smaller: an application that registered
		 * with the tray that went away does not register again, so its
		 * icon is missing until it restarts.  Standing down forever
		 * would lose the same icon AND every later one.
		 */
		self->watcher_name_id = g_bus_own_name_on_connection(self->conn,
			SNW_NAME, G_BUS_NAME_OWNER_FLAGS_NONE,
			on_watcher_name_acquired, on_watcher_name_lost, self, NULL);

		/* And a host name, which is what an application looks for
		 * before deciding anybody is listening. */
		{
			g_autofree gchar *host =
				g_strdup_printf("org.kde.StatusNotifierHost-%d", getpid());

			self->host_name_id = g_bus_own_name_on_connection(self->conn,
				host, G_BUS_NAME_OWNER_FLAGS_NONE, NULL, NULL, NULL, NULL);
		}
	}

	g_main_loop_run(self->loop);

	if (self->watcher_name_id != 0)
		g_bus_unown_name(self->watcher_name_id);
	if (self->host_name_id != 0)
		g_bus_unown_name(self->host_name_id);
	self->watcher_name_id = self->host_name_id = 0;
	if (self->reg_id != 0)
		g_dbus_connection_unregister_object(self->conn, self->reg_id);
	self->reg_id = 0;

	/* Unowning and unregistering release their references to the
	 * connection from this context; turn it until they have, or the
	 * connection outlives the thread that owns it. */
	while (g_main_context_iteration(self->ctx, FALSE))
		;
	g_clear_object(&self->conn);
	g_main_context_pop_thread_default(self->ctx);
	return NULL;
}

/* ── Actions, handed to the bus thread ───────────────────────────── */

typedef struct {
	GowlTray *tray;
	gchar    *key;
	gchar    *method;
	gchar    *orientation;
	gint      a;
	gint      b;
} Action;

static void
action_free(gpointer data)
{
	Action *act = data;

	g_free(act->key);
	g_free(act->method);
	g_free(act->orientation);
	g_object_unref(act->tray);
	g_free(act);
}

static gboolean
action_run(gpointer data)
{
	Action   *act = data;
	GowlTray *self = act->tray;
	g_autofree gchar *bus = NULL;
	g_autofree gchar *path = NULL;
	g_autofree gchar *menu_path = NULL;
	TrayEntry *e;

	if (self->conn == NULL)
		return G_SOURCE_REMOVE;

	g_mutex_lock(&self->lock);
	e = g_hash_table_lookup(self->entries, act->key);
	if (e != NULL) {
		bus = g_strdup(e->item.service);
		path = g_strdup(e->item.path);
		menu_path = g_strdup(e->item.menu_path);
	}
	g_mutex_unlock(&self->lock);
	if (bus == NULL)
		return G_SOURCE_REMOVE;

	if (g_strcmp0(act->method, "@menu-refresh") == 0) {
		tray_read_menu(self, act->key);
		return G_SOURCE_REMOVE;
	}
	if (g_strcmp0(act->method, "@menu-click") == 0) {
		if (menu_path == NULL)
			return G_SOURCE_REMOVE;
		g_dbus_connection_call(self->conn, bus, menu_path, MENU_IFACE,
			"Event",
			g_variant_new("(isvu)", act->a, "clicked",
			              g_variant_new_int32(0),
			              (guint32)(g_get_real_time() / G_USEC_PER_SEC)),
			NULL, G_DBUS_CALL_FLAGS_NONE, GOWL_TRAY_CALL_TIMEOUT_MS,
			NULL, NULL, NULL);
		return G_SOURCE_REMOVE;
	}

	/*
	 * Fire and forget, on both spellings of the interface.  Sending to
	 * the one the application does not implement costs an error reply
	 * nobody reads, and working out which it is first would cost a
	 * round trip on every click.
	 */
	if (g_strcmp0(act->method, "Scroll") == 0) {
		g_dbus_connection_call(self->conn, bus, path, SNI_IFACE_KDE,
			"Scroll", g_variant_new("(is)", act->a, act->orientation),
			NULL, G_DBUS_CALL_FLAGS_NONE, GOWL_TRAY_CALL_TIMEOUT_MS,
			NULL, NULL, NULL);
		g_dbus_connection_call(self->conn, bus, path, SNI_IFACE_FDO,
			"Scroll", g_variant_new("(is)", act->a, act->orientation),
			NULL, G_DBUS_CALL_FLAGS_NONE, GOWL_TRAY_CALL_TIMEOUT_MS,
			NULL, NULL, NULL);
		return G_SOURCE_REMOVE;
	}

	g_dbus_connection_call(self->conn, bus, path, SNI_IFACE_KDE,
		act->method, g_variant_new("(ii)", act->a, act->b), NULL,
		G_DBUS_CALL_FLAGS_NONE, GOWL_TRAY_CALL_TIMEOUT_MS,
		NULL, NULL, NULL);
	g_dbus_connection_call(self->conn, bus, path, SNI_IFACE_FDO,
		act->method, g_variant_new("(ii)", act->a, act->b), NULL,
		G_DBUS_CALL_FLAGS_NONE, GOWL_TRAY_CALL_TIMEOUT_MS,
		NULL, NULL, NULL);
	return G_SOURCE_REMOVE;
}

static void
tray_dispatch(GowlTray *self, const gchar *key, const gchar *method,
              gint a, gint b, const gchar *orientation)
{
	Action *act;

	g_return_if_fail(GOWL_IS_TRAY(self));
	if (key == NULL || self->ctx == NULL)
		return;

	act = g_new0(Action, 1);
	act->tray        = g_object_ref(self);
	act->key         = g_strdup(key);
	act->method      = g_strdup(method);
	act->orientation = g_strdup(orientation);
	act->a           = a;
	act->b           = b;
	g_main_context_invoke_full(self->ctx, G_PRIORITY_DEFAULT,
	                           action_run, act, action_free);
}

void
gowl_tray_activate(GowlTray *self, const gchar *key, gint x, gint y)
{
	tray_dispatch(self, key, "Activate", x, y, NULL);
}

void
gowl_tray_secondary_activate(GowlTray *self, const gchar *key,
                             gint x, gint y)
{
	tray_dispatch(self, key, "SecondaryActivate", x, y, NULL);
}

void
gowl_tray_context_menu(GowlTray *self, const gchar *key, gint x, gint y)
{
	tray_dispatch(self, key, "ContextMenu", x, y, NULL);
}

void
gowl_tray_scroll(GowlTray *self, const gchar *key, gint delta,
                 const gchar *orientation)
{
	tray_dispatch(self, key, "Scroll", delta, 0,
	              orientation != NULL ? orientation : "vertical");
}

void
gowl_tray_menu_refresh(GowlTray *self, const gchar *key)
{
	tray_dispatch(self, key, "@menu-refresh", 0, 0, NULL);
}

void
gowl_tray_menu_clicked(GowlTray *self, const gchar *key, gint id)
{
	tray_dispatch(self, key, "@menu-click", id, 0, NULL);
}

/* ── What the compositor thread reads ────────────────────────────── */

GPtrArray *
gowl_tray_dup_items(GowlTray *self)
{
	GPtrArray *out;
	guint      i;

	g_return_val_if_fail(GOWL_IS_TRAY(self), NULL);

	out = g_ptr_array_new_with_free_func(
		(GDestroyNotify)gowl_tray_item_free);
	g_mutex_lock(&self->lock);
	for (i = 0; i < self->order->len; i++) {
		TrayEntry *e = g_hash_table_lookup(self->entries,
			g_ptr_array_index(self->order, i));

		if (e != NULL)
			g_ptr_array_add(out, gowl_tray_item_copy(&e->item));
	}
	g_mutex_unlock(&self->lock);
	return out;
}

guint
gowl_tray_get_serial(GowlTray *self)
{
	guint serial;

	g_return_val_if_fail(GOWL_IS_TRAY(self), 0);
	g_mutex_lock(&self->lock);
	serial = self->serial;
	g_mutex_unlock(&self->lock);
	return serial;
}

gboolean
gowl_tray_is_serving(GowlTray *self)
{
	gboolean serving;

	g_return_val_if_fail(GOWL_IS_TRAY(self), FALSE);
	g_mutex_lock(&self->lock);
	serving = self->serving;
	g_mutex_unlock(&self->lock);
	return serving;
}

GowlTrayMenuItem *
gowl_tray_dup_menu(GowlTray *self, const gchar *key)
{
	GowlTrayMenuItem *copy = NULL;
	TrayEntry        *e;

	g_return_val_if_fail(GOWL_IS_TRAY(self), NULL);
	if (key == NULL)
		return NULL;

	g_mutex_lock(&self->lock);
	e = g_hash_table_lookup(self->entries, key);
	if (e != NULL && e->menu != NULL)
		copy = gowl_tray_menu_item_copy(e->menu);
	g_mutex_unlock(&self->lock);
	return copy;
}

/* ── Lifecycle ───────────────────────────────────────────────────── */

gboolean
gowl_tray_start(GowlTray *self, struct wl_event_loop *loop, GError **error)
{
	g_return_val_if_fail(GOWL_IS_TRAY(self), FALSE);

	if (self->thread != NULL)
		return TRUE;

	self->wake_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
	if (self->wake_fd < 0) {
		g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errno),
		            "eventfd: %s", g_strerror(errno));
		return FALSE;
	}
	if (loop != NULL) {
		self->wake_source = wl_event_loop_add_fd(loop, self->wake_fd,
			WL_EVENT_READABLE, on_wake, self);
	}

	self->ctx  = g_main_context_new();
	self->loop = g_main_loop_new(self->ctx, FALSE);
	self->thread = g_thread_try_new("gowl-tray", bus_thread, self, error);
	if (self->thread == NULL) {
		g_clear_pointer(&self->loop, g_main_loop_unref);
		g_clear_pointer(&self->ctx, g_main_context_unref);
		return FALSE;
	}
	return TRUE;
}

void
gowl_tray_stop(GowlTray *self)
{
	g_return_if_fail(GOWL_IS_TRAY(self));

	if (self->thread != NULL) {
		self->stopping = TRUE;
		g_main_loop_quit(self->loop);
		g_thread_join(self->thread);
		self->thread = NULL;
		g_clear_pointer(&self->loop, g_main_loop_unref);
		g_clear_pointer(&self->ctx, g_main_context_unref);
	}
	if (self->wake_source != NULL) {
		wl_event_source_remove(self->wake_source);
		self->wake_source = NULL;
	}
	if (self->wake_fd >= 0) {
		close(self->wake_fd);
		self->wake_fd = -1;
	}
	g_mutex_lock(&self->lock);
	self->serving = FALSE;
	g_hash_table_remove_all(self->entries);
	g_ptr_array_set_size(self->order, 0);
	g_mutex_unlock(&self->lock);
}

GowlTray *
gowl_tray_new(void)
{
	return g_object_new(GOWL_TYPE_TRAY, NULL);
}

GowlTray *
gowl_tray_get_default(void)
{
	static GowlTray *singleton = NULL;
	static gsize     once = 0;

	if (g_once_init_enter(&once)) {
		singleton = gowl_tray_new();
		g_once_init_leave(&once, 1);
	}
	return singleton;
}

static void
gowl_tray_finalize(GObject *object)
{
	GowlTray *self = GOWL_TRAY(object);

	gowl_tray_stop(self);
	g_hash_table_unref(self->entries);
	g_ptr_array_unref(self->order);
	g_mutex_clear(&self->lock);
	G_OBJECT_CLASS(gowl_tray_parent_class)->finalize(object);
}

static void
gowl_tray_class_init(GowlTrayClass *klass)
{
	G_OBJECT_CLASS(klass)->finalize = gowl_tray_finalize;

	/**
	 * GowlTray::changed:
	 *
	 * Anything about the register moved.  Raised on the COMPOSITOR
	 * thread, from the Wayland event loop, so a handler may touch the
	 * scene and may run Lisp.
	 */
	signals[SIGNAL_CHANGED] = g_signal_new("changed",
		G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL,
		NULL, G_TYPE_NONE, 0);
}

static void
gowl_tray_init(GowlTray *self)
{
	g_mutex_init(&self->lock);
	self->wake_fd = -1;
	self->entries = g_hash_table_new_full(g_str_hash, g_str_equal,
	                                      g_free, tray_entry_free);
	self->order = g_ptr_array_new_with_free_func(g_free);
}
