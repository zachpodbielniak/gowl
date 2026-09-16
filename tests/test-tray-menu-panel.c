/* test-tray-menu-panel.c -- the tray's menu, as the panel it becomes
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * test-tray covers the register: what an application publishes and what
 * gowl makes of it.  This covers the half that reaches a person ---
 * right-clicking a tray icon and getting a menu that works.  It drives
 * the real plugin out of the real bar.so against a real bus, because
 * both of the faults it exists for were in the seam between the two and
 * neither side looked wrong on its own.
 *
 * What is asserted:
 *
 *   THE PANEL COMES BACK FOR THE MENU.  A menu is asked for at the
 *   moment the panel opens and arrives from the bus thread some time
 *   after, and a panel is built once.  Nothing asked for it to be built
 *   again, so the "asking the application" placeholder stayed up for
 *   ever: the menu appeared only on the NEXT right click, after a click
 *   elsewhere to dismiss the first one.
 *
 *   THE ROWS CAN BE CLICKED.  They were built as panel FIELDS, which
 *   are a label and a value: drawn muted and given no hit region at
 *   all.  Every row of every tray menu was grey and inert whatever id
 *   it carried.  A row has to be a ROW.
 *
 *   A DISABLED ROW STAYS INERT.  The application said so, and the way
 *   to honour that is the panel's own disabled flag --- faint, and no
 *   hit region --- not a row that merely looks ordinary.
 */

#include <glib.h>
#include <gio/gio.h>
#include <gmodule.h>
#include <pango/pangocairo.h>
#include <string.h>

#include "tray/gowl-tray.h"
#include "barkit/gowl-bar-plugin.h"
#include "barkit/gowl-bar-registry.h"
#include "barkit/gowl-bar-panel.h"
#include "barkit/gowl-bar-panel-render.h"
#include "barkit/gowl-bar-theme.h"
#include "barkit/gowl-bar-host.h"

#ifndef GOWL_TEST_BAR_MODULE
#define GOWL_TEST_BAR_MODULE "build/release/modules/bar.so"
#endif

#define FAKE_PATH "/StatusNotifierItem"
#define FAKE_MENU "/MenuBar"
#define PANEL_W   (320)

/* ── A host that only remembers what it was asked for ─────────────── */

typedef struct {
	GObject  parent;
	guint    opened;    /* open_panel calls */
	guint    refreshed; /* request_panel_refresh calls */
} StubHost;

typedef struct { GObjectClass parent; } StubHostClass;

static void stub_host_iface_init(GowlBarHostInterface *iface);

G_DEFINE_TYPE_WITH_CODE(StubHost, stub_host, G_TYPE_OBJECT,
	G_IMPLEMENT_INTERFACE(GOWL_TYPE_BAR_HOST, stub_host_iface_init))

static void
stub_open_panel(GowlBarHost *host, const gchar *plugin_id)
{
	(void)plugin_id;
	((StubHost *)host)->opened++;
}

static void
stub_request_panel_refresh(GowlBarHost *host, GowlBarPlugin *plugin)
{
	(void)plugin;
	((StubHost *)host)->refreshed++;
}

static void
stub_host_iface_init(GowlBarHostInterface *iface)
{
	iface->open_panel            = stub_open_panel;
	iface->request_panel_refresh = stub_request_panel_refresh;
}

static void stub_host_class_init(StubHostClass *k) { (void)k; }
static void stub_host_init(StubHost *s) { (void)s; }

/* ── The fake application ─────────────────────────────────────────── */

typedef struct {
	GTestDBus       *bus;
	GDBusConnection *conn;
	guint            name_id;
	guint            item_reg;
	guint            menu_reg;
	gint             menu_event;   /* last Event id, -1 for none */

	GowlBarRegistry *registry;
	GowlBarPlugin   *plugin;
	StubHost        *host;
	GModule         *bar_so;
} Fixture;

static const gchar fake_xml[] =
	"<node>"
	"  <interface name='org.kde.StatusNotifierItem'>"
	"    <method name='Activate'>"
	"      <arg type='i' name='x' direction='in'/>"
	"      <arg type='i' name='y' direction='in'/>"
	"    </method>"
	"    <property name='Id' type='s' access='read'/>"
	"  </interface>"
	"  <interface name='com.canonical.dbusmenu'>"
	"    <method name='GetLayout'>"
	"      <arg type='i' name='parentId' direction='in'/>"
	"      <arg type='i' name='depth' direction='in'/>"
	"      <arg type='as' name='props' direction='in'/>"
	"      <arg type='u' name='revision' direction='out'/>"
	"      <arg type='(ia{sv}av)' name='layout' direction='out'/>"
	"    </method>"
	"    <method name='AboutToShow'>"
	"      <arg type='i' name='id' direction='in'/>"
	"      <arg type='b' name='needUpdate' direction='out'/>"
	"    </method>"
	"    <method name='Event'>"
	"      <arg type='i' name='id' direction='in'/>"
	"      <arg type='s' name='eventId' direction='in'/>"
	"      <arg type='v' name='data' direction='in'/>"
	"      <arg type='u' name='timestamp' direction='in'/>"
	"    </method>"
	"  </interface>"
	"</node>";

/*
 * Three rows: one ordinary, one the application has disabled, and one
 * separator.  The disabled one is the whole reason a menu row cannot
 * simply be "clickable".
 */
static GVariant *
fake_layout(void)
{
	GVariantBuilder kids, none, open_p, off_p, sep_p, root_p;
	GVariant *open, *off, *sep;

	g_variant_builder_init(&none, G_VARIANT_TYPE("av"));
	g_variant_builder_init(&open_p, G_VARIANT_TYPE("a{sv}"));
	g_variant_builder_add(&open_p, "{sv}", "label",
	                      g_variant_new_string("_Open Bridge"));
	open = g_variant_new("(ia{sv}av)", 14, &open_p, &none);

	g_variant_builder_init(&none, G_VARIANT_TYPE("av"));
	g_variant_builder_init(&sep_p, G_VARIANT_TYPE("a{sv}"));
	g_variant_builder_add(&sep_p, "{sv}", "type",
	                      g_variant_new_string("separator"));
	sep = g_variant_new("(ia{sv}av)", 15, &sep_p, &none);

	g_variant_builder_init(&none, G_VARIANT_TYPE("av"));
	g_variant_builder_init(&off_p, G_VARIANT_TYPE("a{sv}"));
	g_variant_builder_add(&off_p, "{sv}", "label",
	                      g_variant_new_string("Connecting"));
	g_variant_builder_add(&off_p, "{sv}", "enabled",
	                      g_variant_new_boolean(FALSE));
	off = g_variant_new("(ia{sv}av)", 16, &off_p, &none);

	g_variant_builder_init(&kids, G_VARIANT_TYPE("av"));
	g_variant_builder_add(&kids, "v", open);
	g_variant_builder_add(&kids, "v", sep);
	g_variant_builder_add(&kids, "v", off);
	g_variant_builder_init(&root_p, G_VARIANT_TYPE("a{sv}"));
	return g_variant_new("(u(ia{sv}av))", 1u, 0, &root_p, &kids);
}

static void
fake_method(GDBusConnection *conn, const gchar *sender, const gchar *path,
            const gchar *iface, const gchar *method, GVariant *params,
            GDBusMethodInvocation *inv, gpointer data)
{
	Fixture *f = data;

	(void)conn; (void)sender; (void)path; (void)iface;

	if (g_strcmp0(method, "GetLayout") == 0) {
		g_dbus_method_invocation_return_value(inv, fake_layout());
		return;
	}
	if (g_strcmp0(method, "AboutToShow") == 0) {
		g_dbus_method_invocation_return_value(inv,
			g_variant_new("(b)", FALSE));
		return;
	}
	if (g_strcmp0(method, "Event") == 0) {
		g_variant_get_child(params, 0, "i", &f->menu_event);
		g_dbus_method_invocation_return_value(inv, NULL);
		return;
	}
	g_dbus_method_invocation_return_value(inv, NULL);
}

static GVariant *
fake_get_property(GDBusConnection *conn, const gchar *sender,
                  const gchar *path, const gchar *iface, const gchar *prop,
                  GError **error, gpointer data)
{
	(void)conn; (void)sender; (void)path; (void)iface; (void)data;
	(void)error;
	if (g_strcmp0(prop, "Id") == 0)
		return g_variant_new_string("fake");
	return NULL;
}

static const GDBusInterfaceVTable fake_vtable = {
	fake_method, fake_get_property, NULL, { 0 }
};

/* Answer GetAll so the item registers with something in it. */
static GDBusMessage *
fake_filter(GDBusConnection *conn, GDBusMessage *message, gboolean incoming,
            gpointer data)
{
	GVariantBuilder b;
	GDBusMessage *reply;

	(void)data;
	if (!incoming
	    || g_dbus_message_get_message_type(message)
	       != G_DBUS_MESSAGE_TYPE_METHOD_CALL
	    || g_strcmp0(g_dbus_message_get_interface(message),
	                 "org.freedesktop.DBus.Properties") != 0
	    || g_strcmp0(g_dbus_message_get_member(message), "GetAll") != 0
	    || g_strcmp0(g_dbus_message_get_path(message), FAKE_PATH) != 0)
		return message;

	g_variant_builder_init(&b, G_VARIANT_TYPE("a{sv}"));
	g_variant_builder_add(&b, "{sv}", "Id", g_variant_new_string("fake"));
	g_variant_builder_add(&b, "{sv}", "Status",
	                      g_variant_new_string("Active"));
	g_variant_builder_add(&b, "{sv}", "Menu",
	                      g_variant_new_object_path(FAKE_MENU));

	reply = g_dbus_message_new_method_reply(message);
	g_dbus_message_set_body(reply,
		g_variant_new_tuple(&(GVariant *){
			g_variant_builder_end(&b) }, 1));
	g_dbus_connection_send_message(conn, reply,
		G_DBUS_SEND_MESSAGE_FLAGS_NONE, NULL, NULL);
	g_object_unref(reply);
	g_object_unref(message);
	return NULL;
}

/* ── Fixture ──────────────────────────────────────────────────────── */

typedef void (*RegisterFn)(GowlBarRegistry *);

static gboolean
fixture_up(Fixture *f)
{
	GError *error = NULL;
	GDBusNodeInfo *info;
	RegisterFn reg = NULL;

	memset(f, 0, sizeof(*f));
	f->menu_event = -1;

	f->bus = g_test_dbus_new(G_TEST_DBUS_NONE);
	g_test_dbus_up(f->bus);

	f->conn = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &error);
	g_assert_no_error(error);

	/* The plugin reads the DEFAULT tray, so that is the one to serve. */
	g_assert_true(gowl_tray_start(gowl_tray_get_default(), NULL, &error));
	g_assert_no_error(error);

	info = g_dbus_node_info_new_for_xml(fake_xml, &error);
	g_assert_no_error(error);
	f->item_reg = g_dbus_connection_register_object(f->conn, FAKE_PATH,
		info->interfaces[0], &fake_vtable, f, NULL, &error);
	g_assert_no_error(error);
	f->menu_reg = g_dbus_connection_register_object(f->conn, FAKE_MENU,
		info->interfaces[1], &fake_vtable, f, NULL, &error);
	g_assert_no_error(error);
	g_dbus_node_info_unref(info);
	g_dbus_connection_add_filter(f->conn, fake_filter, f, NULL);
	f->name_id = g_bus_own_name_on_connection(f->conn,
		"org.example.FakeTrayPanel", G_BUS_NAME_OWNER_FLAGS_NONE,
		NULL, NULL, NULL, NULL);

	/*
	 * The name is acquired on the bus thread, and an application can
	 * only register with a watcher that is already there.
	 */
	{
		gint64 deadline = g_get_monotonic_time() + 3 * G_USEC_PER_SEC;

		while (!gowl_tray_is_serving(gowl_tray_get_default())
		       && g_get_monotonic_time() < deadline) {
			g_main_context_iteration(NULL, FALSE);
			g_usleep(2000);
		}
		if (!gowl_tray_is_serving(gowl_tray_get_default()))
			return FALSE;
	}

	/* Publishing is not registering: an application has to tell the
	 * watcher it is there. */
	{
		g_autoptr(GVariant) reply = NULL;

		reply = g_dbus_connection_call_sync(f->conn,
			"org.kde.StatusNotifierWatcher", "/StatusNotifierWatcher",
			"org.kde.StatusNotifierWatcher",
			"RegisterStatusNotifierItem",
			g_variant_new("(s)", "org.example.FakeTrayPanel"),
			NULL, G_DBUS_CALL_FLAGS_NONE, 3000, NULL, &error);
		g_assert_no_error(error);
	}

	/* The tray widget is a builtin of bar.so, and bar.so exports the
	 * call that registers it.  Nothing else can reach it. */
	f->bar_so = g_module_open(GOWL_TEST_BAR_MODULE, G_MODULE_BIND_LAZY);
	if (f->bar_so == NULL)
		return FALSE;
	if (!g_module_symbol(f->bar_so, "bar_register_tray_plugins",
	                     (gpointer *)&reg) || reg == NULL)
		return FALSE;

	f->registry = gowl_bar_registry_new(NULL);
	reg(f->registry);
	f->plugin = gowl_bar_registry_instantiate(f->registry, "tray", &error);
	if (f->plugin == NULL)
		return FALSE;
	g_assert_true(gowl_bar_plugin_activate(f->plugin, &error));

	f->host = g_object_new(stub_host_get_type(), NULL);
	gowl_bar_plugin_set_host(f->plugin, GOWL_BAR_HOST(f->host));
	return TRUE;
}

static void
fixture_down(Fixture *f)
{
	if (f->plugin != NULL) {
		gowl_bar_plugin_deactivate(f->plugin);
		g_clear_object(&f->plugin);
	}
	g_clear_object(&f->registry);
	g_clear_object(&f->host);
	if (f->name_id != 0)
		g_bus_unown_name(f->name_id);
	if (f->item_reg != 0)
		g_dbus_connection_unregister_object(f->conn, f->item_reg);
	if (f->menu_reg != 0)
		g_dbus_connection_unregister_object(f->conn, f->menu_reg);
	gowl_tray_stop(gowl_tray_get_default());
	g_clear_object(&f->conn);
	g_test_dbus_down(f->bus);
	g_clear_object(&f->bus);
	if (f->bar_so != NULL)
		g_module_close(f->bar_so);
}

static gboolean
turn_until(gboolean (*pred)(Fixture *), Fixture *f, gint ms)
{
	gint64 deadline = g_get_monotonic_time() + (gint64)ms * 1000;

	while (g_get_monotonic_time() < deadline) {
		if (pred(f))
			return TRUE;
		g_main_context_iteration(NULL, FALSE);
		g_usleep(2000);
	}
	return pred(f);
}

static gboolean
item_arrived(Fixture *f)
{
	g_autoptr(GPtrArray) items = NULL;

	(void)f;
	items = gowl_tray_dup_items(gowl_tray_get_default());
	return items != NULL && items->len > 0;
}

static const gchar *
the_key(void)
{
	static gchar *key = NULL;
	g_autoptr(GPtrArray) items = NULL;

	items = gowl_tray_dup_items(gowl_tray_get_default());
	if (items == NULL || items->len == 0)
		return NULL;
	g_free(key);
	key = g_strdup(((GowlTrayItem *)g_ptr_array_index(items, 0))->key);
	return key;
}

static gboolean
menu_arrived(Fixture *f)
{
	g_autoptr(GowlTrayMenuItem) m = NULL;
	const gchar *key = the_key();

	(void)f;
	if (key == NULL)
		return FALSE;
	m = gowl_tray_dup_menu(gowl_tray_get_default(), key);
	return m != NULL && m->children != NULL && m->children->len > 0;
}

/* Lay the plugin out, then right-click its one icon. */
static void
right_click_the_icon(Fixture *f)
{
	g_autoptr(PangoLayout) layout = NULL;
	cairo_surface_t *s;
	cairo_t *cr;
	GowlBarTheme *theme;
	gint w;

	s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 200, 28);
	cr = cairo_create(s);
	layout = pango_cairo_create_layout(cr);
	theme = gowl_bar_theme_new();

	/* measure is what fills the widget's item list and slot width. */
	w = gowl_bar_plugin_measure(f->plugin, layout, theme, 28);
	g_assert_cmpint(w, >, 0);

	/* BTN_RIGHT is 0x111. */
	g_assert_true(gowl_bar_plugin_on_click(f->plugin, 0x111, 2, 10, 0));

	gowl_bar_theme_free(theme);
	cairo_destroy(cr);
	cairo_surface_destroy(s);
}

/* Render a panel and report how many rows offered a hit region. */
static guint
hit_count(GowlBarPanel *panel)
{
	cairo_surface_t *s;
	cairo_t *cr;
	PangoLayout *layout;
	GowlBarTheme *theme;
	GowlBarPanelRenderCtx ctx;
	guint n;

	s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, PANEL_W, 600);
	cr = cairo_create(s);
	layout = pango_cairo_create_layout(cr);
	theme = gowl_bar_theme_new();

	gowl_bar_panel_measure(panel, layout, theme, PANEL_W);
	gowl_bar_panel_render_ctx_init(&ctx, PANEL_W);
	ctx.hits = g_array_new(FALSE, FALSE, sizeof(GowlBarHitRect));
	gowl_bar_panel_render(panel, cr, layout, theme, &ctx);
	n = ctx.hits->len;

	g_array_unref(ctx.hits);
	gowl_bar_theme_free(theme);
	g_object_unref(layout);
	cairo_destroy(cr);
	cairo_surface_destroy(s);
	return n;
}

/* ------------------------------------------------------------------ */

static void
test_the_panel_comes_back_for_the_menu(void)
{
	Fixture f;
	g_autoptr(GowlBarPanel) first = NULL;
	g_autoptr(GowlBarPanel) second = NULL;
	GString *sig;
	guint before;

	if (!fixture_up(&f)) {
		g_test_skip("bar.so is not built");
		fixture_down(&f);
		return;
	}
	g_assert_true(turn_until(item_arrived, &f, 3000));

	right_click_the_icon(&f);
	g_assert_cmpuint(f.host->opened, ==, 1);

	/* Built before the menu could possibly have arrived: the
	 * placeholder, with nothing to click. */
	first = gowl_bar_plugin_build_panel(f.plugin);
	g_assert_nonnull(first);
	g_assert_cmpuint(hit_count(first), ==, 0);

	g_assert_true(turn_until(menu_arrived, &f, 3000));

	/*
	 * The bar asks every plugin for its signature when the register
	 * says something moved.  THAT is the moment the tray has to notice
	 * its menu landed and ask for the panel to be built again --- and
	 * the moment it did not, which is why the placeholder stayed up
	 * until the panel was closed and reopened.
	 */
	before = f.host->refreshed;
	sig = g_string_new(NULL);
	gowl_bar_plugin_signature(f.plugin, sig);
	g_string_free(sig, TRUE);
	g_assert_cmpuint(f.host->refreshed, >, before);

	/* And the rebuild has the menu in it. */
	second = gowl_bar_plugin_build_panel(f.plugin);
	g_assert_nonnull(second);
	g_assert_cmpuint(hit_count(second), >, 0);

	fixture_down(&f);
}

static void
test_the_rows_can_be_clicked(void)
{
	Fixture f;
	g_autoptr(GowlBarPanel) panel = NULL;

	if (!fixture_up(&f)) {
		g_test_skip("bar.so is not built");
		fixture_down(&f);
		return;
	}
	g_assert_true(turn_until(item_arrived, &f, 3000));
	right_click_the_icon(&f);
	g_assert_true(turn_until(menu_arrived, &f, 3000));

	panel = gowl_bar_plugin_build_panel(f.plugin);
	g_assert_nonnull(panel);

	/*
	 * Three rows are offered and one of them the application has
	 * disabled, so exactly one hit region: the enabled row.  Built as
	 * panel FIELDS there were none at all, whatever id they carried,
	 * and every tray menu was grey and did nothing.
	 */
	g_assert_cmpuint(hit_count(panel), ==, 1);

	fixture_down(&f);
}

static void
test_clicking_a_row_reaches_the_application(void)
{
	Fixture f;
	g_autoptr(GowlBarPanel) panel = NULL;

	if (!fixture_up(&f)) {
		g_test_skip("bar.so is not built");
		fixture_down(&f);
		return;
	}
	g_assert_true(turn_until(item_arrived, &f, 3000));
	right_click_the_icon(&f);
	g_assert_true(turn_until(menu_arrived, &f, 3000));

	panel = gowl_bar_plugin_build_panel(f.plugin);
	g_assert_nonnull(panel);

	/* Row 14 is the enabled one.  The whole point of the menu is that
	 * this arrives at the application. */
	gowl_bar_plugin_panel_action(f.plugin, "14", -1, 0.0, 1);
	{
		gint64 deadline = g_get_monotonic_time() + 3000000;

		while (f.menu_event != 14
		       && g_get_monotonic_time() < deadline) {
			g_main_context_iteration(NULL, FALSE);
			g_usleep(2000);
		}
	}
	g_assert_cmpint(f.menu_event, ==, 14);

	fixture_down(&f);
}

int
main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/tray-menu-panel/the-panel-comes-back-for-the-menu",
	                test_the_panel_comes_back_for_the_menu);
	g_test_add_func("/tray-menu-panel/the-rows-can-be-clicked",
	                test_the_rows_can_be_clicked);
	g_test_add_func("/tray-menu-panel/clicking-a-row-reaches-the-app",
	                test_clicking_a_row_reaches_the_application);

	return g_test_run();
}
