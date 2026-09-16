/* test-tray.c -- the StatusNotifierItem register
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A tray is a protocol, and every interesting thing about this one is a
 * place where applications disagree with the specification or with each
 * other.  So the fixture is a FAKE TRAY APPLICATION on a bus of its own:
 * it publishes a StatusNotifierItem, registers it the way a real one
 * would, and can then be made to misbehave in each of the specific ways
 * the real ones do.
 *
 * What is asserted, and why none of it can be checked by looking:
 *
 *   REGISTRATION TAKES THREE SHAPES.  The argument is documented as a
 *   service name and applications pass a bus name, an object path, or
 *   the two joined.  A tray that understands only one of them shows an
 *   empty space for a third of the applications on the machine, and
 *   which third depends on which library each linked.
 *
 *   THE INTERFACE HAS TWO NAMES.  An application that publishes the
 *   freedesktop spelling is invisible to a tray that asks only for the
 *   KDE one -- and it is invisible SILENTLY, which is the whole
 *   difficulty: nothing errors, the icon simply never appears.
 *
 *   A PIXMAP SET HAS TO BE CHOSEN FROM.  Applications offer anything
 *   from one 16x16 to a set running to 512, in no promised order.  Both
 *   ends of that are wrong to take: the smallest is scaled up into
 *   mush and the largest is a filter over a quarter of a megabyte to
 *   draw twenty pixels.
 *
 *   AN APPLICATION THAT DIES MUST LEAVE.  Nothing tells a tray that an
 *   item has gone; the bus name simply stops being owned.  A tray that
 *   does not watch for that accumulates dead icons that do nothing when
 *   clicked, which is worse than showing none.
 *
 *   A MENU IS A TREE.  dbusmenu nests, and the labels carry GTK's
 *   mnemonic underscores, which have nothing to attach to here.
 */

#include <glib.h>
#include <gio/gio.h>
#include <string.h>

#include "tray/gowl-tray.h"

#define FAKE_PATH "/StatusNotifierItem"
#define FAKE_MENU "/MenuBar"

typedef struct {
	GTestDBus       *bus;
	GDBusConnection *conn;       /* the fake application's connection */
	GowlTray        *tray;
	guint            name_id;
	guint            item_reg;
	guint            menu_reg;
	gchar           *iface;      /* which spelling to publish */
	gboolean         many_sizes;
	guint            activated;  /* Activate calls seen */
	gint             menu_event; /* last Event id, -1 for none */
} Fixture;

/* ── The fake application ────────────────────────────────────────── */

static const gchar item_xml[] =
	"<node>"
	"  <interface name='org.kde.StatusNotifierItem'>"
	"    <method name='Activate'>"
	"      <arg type='i' name='x' direction='in'/>"
	"      <arg type='i' name='y' direction='in'/>"
	"    </method>"
	"    <method name='SecondaryActivate'>"
	"      <arg type='i' name='x' direction='in'/>"
	"      <arg type='i' name='y' direction='in'/>"
	"    </method>"
	"    <method name='ContextMenu'>"
	"      <arg type='i' name='x' direction='in'/>"
	"      <arg type='i' name='y' direction='in'/>"
	"    </method>"
	"    <property name='Id' type='s' access='read'/>"
	"  </interface>"
	"  <interface name='org.freedesktop.StatusNotifierItem'>"
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
	"      <arg type='b' name='need' direction='out'/>"
	"    </method>"
	"    <method name='Event'>"
	"      <arg type='i' name='id' direction='in'/>"
	"      <arg type='s' name='eventId' direction='in'/>"
	"      <arg type='v' name='data' direction='in'/>"
	"      <arg type='u' name='timestamp' direction='in'/>"
	"    </method>"
	"  </interface>"
	"</node>";

/* One pixmap entry: width, height and w*h*4 bytes of ARGB. */
static GVariant *
fake_pixmap(gint w, gint h, guint8 fill)
{
	GVariantBuilder bytes;
	gint i;

	g_variant_builder_init(&bytes, G_VARIANT_TYPE("ay"));
	for (i = 0; i < w * h * 4; i++)
		g_variant_builder_add(&bytes, "y", fill);
	return g_variant_new("(ii@ay)", w, h, g_variant_builder_end(&bytes));
}

static GVariant *
fake_props(Fixture *f)
{
	GVariantBuilder b;
	GVariantBuilder pix;

	g_variant_builder_init(&b, G_VARIANT_TYPE("a{sv}"));
	g_variant_builder_add(&b, "{sv}", "Id", g_variant_new_string("fake"));
	g_variant_builder_add(&b, "{sv}", "Title",
	                      g_variant_new_string("Fake Tray App"));
	g_variant_builder_add(&b, "{sv}", "Status",
	                      g_variant_new_string("Active"));
	g_variant_builder_add(&b, "{sv}", "Category",
	                      g_variant_new_string("ApplicationStatus"));
	g_variant_builder_add(&b, "{sv}", "IconName",
	                      g_variant_new_string("dialog-information"));
	g_variant_builder_add(&b, "{sv}", "ItemIsMenu",
	                      g_variant_new_boolean(TRUE));
	g_variant_builder_add(&b, "{sv}", "Menu",
	                      g_variant_new_object_path(FAKE_MENU));
	g_variant_builder_add(&b, "{sv}", "ToolTip",
		g_variant_new("(sa(iiay)ss)", "", NULL, "Tip title", "Tip body"));

	g_variant_builder_init(&pix, G_VARIANT_TYPE("a(iiay)"));
	if (f->many_sizes) {
		/* Deliberately out of order, and spanning the decision. */
		g_variant_builder_add_value(&pix, fake_pixmap(64, 64, 0x40));
		g_variant_builder_add_value(&pix, fake_pixmap(16, 16, 0x10));
		g_variant_builder_add_value(&pix, fake_pixmap(32, 32, 0x20));
	} else {
		g_variant_builder_add_value(&pix, fake_pixmap(16, 16, 0x10));
	}
	g_variant_builder_add(&b, "{sv}", "IconPixmap",
	                      g_variant_builder_end(&pix));
	return g_variant_builder_end(&b);
}

/*
 * The menu: a root with three rows, one of them a submenu, and labels
 * carrying the mnemonic underscores a GTK application really sends.
 */
static GVariant *
fake_layout(void)
{
	GVariantBuilder kids, sub_kids, props, sub_props, leaf_props, sep_props;
	GVariant *leaf, *sub, *sep, *open;

	g_variant_builder_init(&leaf_props, G_VARIANT_TYPE("a{sv}"));
	g_variant_builder_add(&leaf_props, "{sv}", "label",
	                      g_variant_new_string("_Preferences"));
	g_variant_builder_init(&sub_kids, G_VARIANT_TYPE("av"));
	leaf = g_variant_new("(ia{sv}av)", 11,
	                     &leaf_props, &sub_kids);

	g_variant_builder_init(&sub_props, G_VARIANT_TYPE("a{sv}"));
	g_variant_builder_add(&sub_props, "{sv}", "label",
	                      g_variant_new_string("_Settings"));
	g_variant_builder_add(&sub_props, "{sv}", "children-display",
	                      g_variant_new_string("submenu"));
	g_variant_builder_init(&sub_kids, G_VARIANT_TYPE("av"));
	g_variant_builder_add(&sub_kids, "v", leaf);
	sub = g_variant_new("(ia{sv}av)", 10, &sub_props, &sub_kids);

	g_variant_builder_init(&sep_props, G_VARIANT_TYPE("a{sv}"));
	g_variant_builder_add(&sep_props, "{sv}", "type",
	                      g_variant_new_string("separator"));
	g_variant_builder_init(&sub_kids, G_VARIANT_TYPE("av"));
	sep = g_variant_new("(ia{sv}av)", 12, &sep_props, &sub_kids);

	g_variant_builder_init(&leaf_props, G_VARIANT_TYPE("a{sv}"));
	g_variant_builder_add(&leaf_props, "{sv}", "label",
	                      g_variant_new_string("_Quit"));
	g_variant_builder_add(&leaf_props, "{sv}", "enabled",
	                      g_variant_new_boolean(FALSE));
	g_variant_builder_init(&sub_kids, G_VARIANT_TYPE("av"));
	open = g_variant_new("(ia{sv}av)", 13, &leaf_props, &sub_kids);

	g_variant_builder_init(&kids, G_VARIANT_TYPE("av"));
	g_variant_builder_add(&kids, "v", sub);
	g_variant_builder_add(&kids, "v", sep);
	g_variant_builder_add(&kids, "v", open);
	g_variant_builder_init(&props, G_VARIANT_TYPE("a{sv}"));
	return g_variant_new("(u(ia{sv}av))", 1u,
	                     0, &props, &kids);
}

static void
fake_method(GDBusConnection *conn, const gchar *sender, const gchar *path,
            const gchar *iface, const gchar *method, GVariant *params,
            GDBusMethodInvocation *inv, gpointer data)
{
	Fixture *f = data;

	(void)conn; (void)sender; (void)path;

	if (g_strcmp0(method, "Activate") == 0) {
		f->activated++;
		g_dbus_method_invocation_return_value(inv, NULL);
		return;
	}
	if (g_strcmp0(method, "SecondaryActivate") == 0
	    || g_strcmp0(method, "ContextMenu") == 0) {
		g_dbus_method_invocation_return_value(inv, NULL);
		return;
	}
	if (g_strcmp0(method, "AboutToShow") == 0) {
		g_dbus_method_invocation_return_value(inv,
			g_variant_new("(b)", FALSE));
		return;
	}
	if (g_strcmp0(method, "GetLayout") == 0) {
		g_dbus_method_invocation_return_value(inv, fake_layout());
		return;
	}
	if (g_strcmp0(method, "Event") == 0) {
		gint id = 0;

		g_variant_get_child(params, 0, "i", &id);
		f->menu_event = id;
		g_dbus_method_invocation_return_value(inv, NULL);
		return;
	}
	(void)iface;
	g_dbus_method_invocation_return_error(inv, G_DBUS_ERROR,
		G_DBUS_ERROR_UNKNOWN_METHOD, "no");
}

/*
 * GetAll is answered by hand rather than through registered properties,
 * because that is what the fixture is FOR: an application publishing
 * only the freedesktop spelling must answer on that name and refuse the
 * other, which is exactly what a GDBus property table would not let us
 * express.
 */
static GVariant *
fake_get_property(GDBusConnection *conn, const gchar *sender,
                  const gchar *path, const gchar *iface, const gchar *prop,
                  GError **error, gpointer data)
{
	(void)conn; (void)sender; (void)path; (void)iface; (void)prop;
	(void)data;
	g_set_error(error, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_PROPERTY, "no");
	return NULL;
}

static const GDBusInterfaceVTable fake_vtable = {
	fake_method, fake_get_property, NULL, { 0 }
};

/* Intercept GetAll so the fixture decides which interface exists. */
static GDBusMessage *
fake_filter(GDBusConnection *conn, GDBusMessage *message, gboolean incoming,
            gpointer data)
{
	Fixture *f = data;
	GVariant *body;
	const gchar *want = NULL;

	if (!incoming
	    || g_dbus_message_get_message_type(message)
	       != G_DBUS_MESSAGE_TYPE_METHOD_CALL
	    || g_strcmp0(g_dbus_message_get_interface(message),
	                 "org.freedesktop.DBus.Properties") != 0
	    || g_strcmp0(g_dbus_message_get_member(message), "GetAll") != 0)
		return message;

	body = g_dbus_message_get_body(message);
	if (body != NULL)
		g_variant_get_child(body, 0, "&s", &want);

	{
		GDBusMessage *reply;

		if (g_strcmp0(want, f->iface) == 0) {
			reply = g_dbus_message_new_method_reply(message);
			g_dbus_message_set_body(reply,
				g_variant_new_tuple(&(GVariant *){ fake_props(f) }, 1));
		} else {
			reply = g_dbus_message_new_method_error(message,
				"org.freedesktop.DBus.Error.UnknownInterface",
				"not this one");
		}
		g_dbus_connection_send_message(conn, reply,
			G_DBUS_SEND_MESSAGE_FLAGS_NONE, NULL, NULL);
		g_object_unref(reply);
	}
	g_object_unref(message);
	return NULL;
}

static void
fixture_app_up(Fixture *f, const gchar *bus_name)
{
	GDBusNodeInfo *info;
	GError *error = NULL;

	info = g_dbus_node_info_new_for_xml(item_xml, &error);
	g_assert_no_error(error);

	f->item_reg = g_dbus_connection_register_object(f->conn, FAKE_PATH,
		info->interfaces[0], &fake_vtable, f, NULL, &error);
	g_assert_no_error(error);
	f->menu_reg = g_dbus_connection_register_object(f->conn, FAKE_MENU,
		info->interfaces[2], &fake_vtable, f, NULL, &error);
	g_assert_no_error(error);
	g_dbus_node_info_unref(info);

	g_dbus_connection_add_filter(f->conn, fake_filter, f, NULL);

	f->name_id = g_bus_own_name_on_connection(f->conn, bus_name,
		G_BUS_NAME_OWNER_FLAGS_NONE, NULL, NULL, NULL, NULL);
}

/* ── Fixture ─────────────────────────────────────────────────────── */

static void
fixture_up(Fixture *f, gconstpointer data)
{
	GError *error = NULL;

	(void)data;
	memset(f, 0, sizeof(*f));
	f->iface = g_strdup("org.kde.StatusNotifierItem");
	f->menu_event = -1;

	f->bus = g_test_dbus_new(G_TEST_DBUS_NONE);
	g_test_dbus_up(f->bus);

	f->conn = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &error);
	g_assert_no_error(error);

	f->tray = gowl_tray_new();
	/* No Wayland loop: nothing here waits on the `changed' signal, and
	 * an event loop the test would have to turn is a second thing that
	 * can hang a test that is already waiting on a bus. */
	g_assert_true(gowl_tray_start(f->tray, NULL, &error));
	g_assert_no_error(error);
}

static void
fixture_down(Fixture *f, gconstpointer data)
{
	(void)data;
	if (f->name_id != 0)
		g_bus_unown_name(f->name_id);
	if (f->item_reg != 0)
		g_dbus_connection_unregister_object(f->conn, f->item_reg);
	if (f->menu_reg != 0)
		g_dbus_connection_unregister_object(f->conn, f->menu_reg);
	gowl_tray_stop(f->tray);
	g_clear_object(&f->tray);
	g_clear_object(&f->conn);
	g_test_dbus_down(f->bus);
	g_clear_object(&f->bus);
	g_free(f->iface);
}

/* Turn the main context until @pred or the deadline.  Everything here
 * crosses a thread and a bus, so nothing is instant. */
static gboolean
wait_until(Fixture *f, gboolean (*pred)(Fixture *), gint ms)
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
have_one_item(Fixture *f)
{
	g_autoptr(GPtrArray) items = gowl_tray_dup_items(f->tray);

	return items->len == 1;
}

static gboolean
have_no_items(Fixture *f)
{
	g_autoptr(GPtrArray) items = gowl_tray_dup_items(f->tray);

	return items->len == 0;
}

static gboolean
was_activated(Fixture *f)
{
	return f->activated > 0;
}

static gboolean
menu_arrived(Fixture *f)
{
	g_autoptr(GowlTrayMenuItem) menu = NULL;
	g_autoptr(GPtrArray) items = gowl_tray_dup_items(f->tray);

	if (items->len == 0)
		return FALSE;
	menu = gowl_tray_dup_menu(f->tray,
		((GowlTrayItem *)g_ptr_array_index(items, 0))->key);
	return menu != NULL && menu->children != NULL;
}

static gboolean
menu_event_seen(Fixture *f)
{
	return f->menu_event >= 0;
}

static void
register_as(Fixture *f, const gchar *argument)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GVariant) reply = NULL;

	reply = g_dbus_connection_call_sync(f->conn,
		"org.kde.StatusNotifierWatcher", "/StatusNotifierWatcher",
		"org.kde.StatusNotifierWatcher", "RegisterStatusNotifierItem",
		g_variant_new("(s)", argument), NULL, G_DBUS_CALL_FLAGS_NONE,
		3000, NULL, &error);
	g_assert_no_error(error);
}

/* Does anybody answer on @name?  Asked of the bus rather than tracked,
   so the test cannot disagree with it. */
static gboolean
name_has_owner(const gchar *name)
{
	g_autoptr(GDBusConnection) conn = NULL;
	g_autoptr(GVariant) reply = NULL;
	gboolean owned = FALSE;

	conn = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);
	if (conn == NULL)
		return FALSE;

	reply = g_dbus_connection_call_sync(conn,
		"org.freedesktop.DBus", "/org/freedesktop/DBus",
		"org.freedesktop.DBus", "NameHasOwner",
		g_variant_new("(s)", name), G_VARIANT_TYPE("(b)"),
		G_DBUS_CALL_FLAGS_NONE, 3000, NULL, NULL);
	if (reply != NULL)
		g_variant_get(reply, "(b)", &owned);
	return owned;
}

/* ── The cases ───────────────────────────────────────────────────── */

/*
 * Logging out and back in is a RACE for the watcher name: the outgoing
 * session's compositor still owns it at the moment the incoming one
 * asks for it.
 *
 * gowl has to stand down right then --- two watchers on one bus is how
 * applications end up registered with the one nobody is displaying ---
 * and then take the name when it comes free, with `serving' following
 * it.  Holding the name while reporting not-serving is the worst of
 * both: nothing draws a tray AND no other tray can take over, which is
 * the state a real session was found in.
 *
 * The holder is a PRIVATE connection.  `g_bus_get_sync' hands out one
 * shared session connection per process, so a holder opened that way
 * would be the tray's own connection and the request would come back
 * ALREADY_OWNER without ever racing anything.
 */
static void
test_the_name_is_picked_up_when_it_comes_free(void)
{
	g_autoptr(GTestDBus) bus = NULL;
	g_autoptr(GDBusConnection) holder = NULL;
	g_autoptr(GError) error = NULL;
	GowlTray *tray;
	guint      name_id;
	gint64     deadline;

	bus = g_test_dbus_new(G_TEST_DBUS_NONE);
	g_test_dbus_up(bus);

	holder = g_dbus_connection_new_for_address_sync(
		g_test_dbus_get_bus_address(bus),
		G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT
		| G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,
		NULL, NULL, &error);
	g_assert_no_error(error);

	/* The outgoing session, still on the bus. */
	name_id = g_bus_own_name_on_connection(holder,
		"org.kde.StatusNotifierWatcher",
		G_BUS_NAME_OWNER_FLAGS_NONE, NULL, NULL, NULL, NULL);
	g_assert_cmpuint(name_id, !=, 0);

	deadline = g_get_monotonic_time() + 3 * G_USEC_PER_SEC;
	while (!name_has_owner("org.kde.StatusNotifierWatcher")
	       && g_get_monotonic_time() < deadline) {
		g_main_context_iteration(NULL, FALSE);
		g_usleep(2000);
	}
	g_assert_true(name_has_owner("org.kde.StatusNotifierWatcher"));

	tray = gowl_tray_new();
	g_assert_true(gowl_tray_start(tray, NULL, &error));
	g_assert_no_error(error);

	/* It must not fight for a name somebody else is answering on. */
	deadline = g_get_monotonic_time() + G_USEC_PER_SEC / 2;
	while (g_get_monotonic_time() < deadline) {
		g_main_context_iteration(NULL, FALSE);
		g_usleep(2000);
	}
	g_assert_false(gowl_tray_is_serving(tray));

	/* And now the outgoing session finally exits. */
	g_bus_unown_name(name_id);

	deadline = g_get_monotonic_time() + 5 * G_USEC_PER_SEC;
	while (!gowl_tray_is_serving(tray)
	       && g_get_monotonic_time() < deadline) {
		g_main_context_iteration(NULL, FALSE);
		g_usleep(2000);
	}
	g_assert_true(gowl_tray_is_serving(tray));

	gowl_tray_stop(tray);
	g_object_unref(tray);
	g_dbus_connection_close_sync(holder, NULL, NULL);
	g_test_dbus_down(bus);
}


static void serving (Fixture *f);

static void
test_the_watcher_is_served(Fixture *f, gconstpointer data)
{
	(void)data;
	/*
	 * Owning the name is the precondition for every other case here, so
	 * this one asserts it on its own and the rest simply wait.  An
	 * application registers with whoever owns the name at the moment it
	 * starts; a tray that acquires it late has already missed everybody.
	 */
	serving(f);
}

static void
serving(Fixture *f)
{
	gint64 deadline = g_get_monotonic_time() + 3 * G_USEC_PER_SEC;

	while (!gowl_tray_is_serving(f->tray)
	       && g_get_monotonic_time() < deadline) {
		g_main_context_iteration(NULL, FALSE);
		g_usleep(2000);
	}
	g_assert_true(gowl_tray_is_serving(f->tray));
}

/*
 * The three registration spellings, each in its own case so a failure
 * names the one that broke.
 */
static void
test_registering_by_bus_name(Fixture *f, gconstpointer data)
{
	g_autoptr(GPtrArray) items = NULL;
	GowlTrayItem *item;

	(void)data;
	serving(f);
	fixture_app_up(f, "org.example.FakeTray");
	register_as(f, "org.example.FakeTray");
	g_assert_true(wait_until(f, have_one_item, 3000));

	items = gowl_tray_dup_items(f->tray);
	item = g_ptr_array_index(items, 0);
	g_assert_cmpstr(item->service, ==, "org.example.FakeTray");
	g_assert_cmpstr(item->path, ==, FAKE_PATH);
	g_assert_cmpstr(item->id, ==, "fake");
	g_assert_cmpstr(item->title, ==, "Fake Tray App");
	g_assert_cmpstr(item->status, ==, "Active");
	g_assert_cmpstr(item->icon_name, ==, "dialog-information");
	g_assert_cmpstr(item->tooltip, ==, "Tip title");
	g_assert_true(item->item_is_menu);
	g_assert_cmpstr(item->menu_path, ==, FAKE_MENU);
}

static void
test_registering_by_object_path(Fixture *f, gconstpointer data)
{
	g_autoptr(GPtrArray) items = NULL;

	(void)data;
	serving(f);
	fixture_app_up(f, "org.example.FakeTray2");
	/*
	 * A bare path says nothing about whose it is, so the only correct
	 * reading is "the sender's" -- and the sender here is the unique
	 * name, not the well-known one the application also owns.
	 */
	register_as(f, FAKE_PATH);
	g_assert_true(wait_until(f, have_one_item, 3000));

	items = gowl_tray_dup_items(f->tray);
	g_assert_cmpstr(((GowlTrayItem *)g_ptr_array_index(items, 0))->path,
	                ==, FAKE_PATH);
	g_assert_cmpstr(((GowlTrayItem *)g_ptr_array_index(items, 0))->id,
	                ==, "fake");
}

static void
test_registering_by_both_joined(Fixture *f, gconstpointer data)
{
	g_autoptr(GPtrArray) items = NULL;
	g_autofree gchar *joined = NULL;

	(void)data;
	serving(f);
	fixture_app_up(f, "org.example.FakeTray3");
	joined = g_strconcat("org.example.FakeTray3", FAKE_PATH, NULL);
	register_as(f, joined);
	g_assert_true(wait_until(f, have_one_item, 3000));

	items = gowl_tray_dup_items(f->tray);
	g_assert_cmpstr(((GowlTrayItem *)g_ptr_array_index(items, 0))->service,
	                ==, "org.example.FakeTray3");
	g_assert_cmpstr(((GowlTrayItem *)g_ptr_array_index(items, 0))->path,
	                ==, FAKE_PATH);
}

static void
test_the_freedesktop_spelling_is_read_too(Fixture *f, gconstpointer data)
{
	g_autoptr(GPtrArray) items = NULL;

	(void)data;
	serving(f);
	/*
	 * An application that linked Ayatana publishes only this one.  A
	 * tray that asks for the KDE name and gives up shows nothing at all
	 * for it -- and shows it without an error anywhere, which is what
	 * makes this worth a case of its own.
	 */
	g_free(f->iface);
	f->iface = g_strdup("org.freedesktop.StatusNotifierItem");
	fixture_app_up(f, "org.example.FakeTrayFdo");
	register_as(f, "org.example.FakeTrayFdo");
	g_assert_true(wait_until(f, have_one_item, 3000));

	items = gowl_tray_dup_items(f->tray);
	g_assert_cmpstr(((GowlTrayItem *)g_ptr_array_index(items, 0))->title,
	                ==, "Fake Tray App");
}

static void
test_the_middle_pixmap_is_taken(Fixture *f, gconstpointer data)
{
	g_autoptr(GPtrArray) items = NULL;
	GowlTrayItem *item;

	(void)data;
	serving(f);
	f->many_sizes = TRUE;
	fixture_app_up(f, "org.example.FakeTrayPix");
	register_as(f, "org.example.FakeTrayPix");
	g_assert_true(wait_until(f, have_one_item, 3000));

	items = gowl_tray_dup_items(f->tray);
	item = g_ptr_array_index(items, 0);

	/*
	 * 16, 32 and 64 were offered out of order.  32 is the smallest that
	 * will not be scaled UP to a bar icon's twenty-odd pixels; 64 is
	 * four times the data for no more detail at that size, and 16 is
	 * the one that looks soft.  The fill byte says which came back.
	 */
	g_assert_cmpint(item->pixmap_width, ==, 32);
	g_assert_cmpint(item->pixmap_height, ==, 32);
	g_assert_nonnull(item->pixmap);
	g_assert_cmpint(item->pixmap[0], ==, 0x20);
}

static void
test_a_lone_small_pixmap_is_still_taken(Fixture *f, gconstpointer data)
{
	g_autoptr(GPtrArray) items = NULL;

	(void)data;
	serving(f);
	/* Only a 16 on offer.  Too small is not a reason to show nothing. */
	fixture_app_up(f, "org.example.FakeTraySmall");
	register_as(f, "org.example.FakeTraySmall");
	g_assert_true(wait_until(f, have_one_item, 3000));

	items = gowl_tray_dup_items(f->tray);
	g_assert_cmpint(
		((GowlTrayItem *)g_ptr_array_index(items, 0))->pixmap_width,
		==, 16);
}

static void
test_an_application_that_leaves_takes_its_icon(Fixture *f, gconstpointer data)
{
	(void)data;
	serving(f);
	fixture_app_up(f, "org.example.FakeTrayGone");
	register_as(f, "org.example.FakeTrayGone");
	g_assert_true(wait_until(f, have_one_item, 3000));

	/*
	 * Nothing announces that an item has gone; the bus name simply
	 * stops being owned.  A tray that does not watch for it keeps a
	 * dead icon that does nothing when clicked, which is worse than an
	 * empty bar because it looks like the application is running.
	 */
	g_bus_unown_name(f->name_id);
	f->name_id = 0;
	g_assert_true(wait_until(f, have_no_items, 3000));
}

static void
test_a_click_reaches_the_application(Fixture *f, gconstpointer data)
{
	g_autoptr(GPtrArray) items = NULL;

	(void)data;
	serving(f);
	fixture_app_up(f, "org.example.FakeTrayClick");
	register_as(f, "org.example.FakeTrayClick");
	g_assert_true(wait_until(f, have_one_item, 3000));

	items = gowl_tray_dup_items(f->tray);
	gowl_tray_activate(f->tray,
		((GowlTrayItem *)g_ptr_array_index(items, 0))->key, 10, 20);
	g_assert_true(wait_until(f, was_activated, 3000));
}

static void
test_the_menu_is_a_tree_without_mnemonics(Fixture *f, gconstpointer data)
{
	g_autoptr(GPtrArray) items = NULL;
	g_autoptr(GowlTrayMenuItem) menu = NULL;
	GowlTrayMenuItem *row;
	const gchar *key;

	(void)data;
	serving(f);
	fixture_app_up(f, "org.example.FakeTrayMenu");
	register_as(f, "org.example.FakeTrayMenu");
	g_assert_true(wait_until(f, have_one_item, 3000));

	items = gowl_tray_dup_items(f->tray);
	key = ((GowlTrayItem *)g_ptr_array_index(items, 0))->key;
	gowl_tray_menu_refresh(f->tray, key);
	g_assert_true(wait_until(f, menu_arrived, 3000));

	menu = gowl_tray_dup_menu(f->tray, key);
	g_assert_nonnull(menu);
	g_assert_nonnull(menu->children);
	g_assert_cmpuint(menu->children->len, ==, 3);

	/* A submenu, kept as a subtree rather than flattened away. */
	row = g_ptr_array_index(menu->children, 0);
	g_assert_cmpstr(row->label, ==, "Settings");
	g_assert_nonnull(row->children);
	g_assert_cmpuint(row->children->len, ==, 1);
	g_assert_cmpstr(
		((GowlTrayMenuItem *)g_ptr_array_index(row->children, 0))->label,
		==, "Preferences");

	row = g_ptr_array_index(menu->children, 1);
	g_assert_true(row->is_separator);

	/* Disabled stays disabled: clicking a greyed row must not act. */
	row = g_ptr_array_index(menu->children, 2);
	g_assert_cmpstr(row->label, ==, "Quit");
	g_assert_false(row->enabled);

	/* And choosing one reaches the application by its dbusmenu id. */
	gowl_tray_menu_clicked(f->tray, key, 13);
	g_assert_true(wait_until(f, menu_event_seen, 3000));
	g_assert_cmpint(f->menu_event, ==, 13);
}

/* ── The pixels ──────────────────────────────────────────────────── */

/*
 * No bus and no fixture: this is arithmetic over bytes, and it is the
 * one part of drawing a tray icon that is wrong in a way that looks
 * like a design choice rather than a bug.
 */
static void
test_a_pixmap_is_converted_for_cairo(void)
{
	GowlTrayItem item;
	cairo_surface_t *surface;
	guint32 *px;
	guint8 raw[8];

	memset(&item, 0, sizeof(item));
	item.pixmap_width = 2;
	item.pixmap_height = 1;
	item.pixmap = raw;

	/* Two pixels, on the wire: A R G B, in that order, on every
	 * machine.  Opaque pure red, then half-transparent pure blue. */
	raw[0] = 0xff; raw[1] = 0xff; raw[2] = 0x00; raw[3] = 0x00;
	raw[4] = 0x80; raw[5] = 0x00; raw[6] = 0x00; raw[7] = 0xff;

	surface = gowl_tray_item_argb32(&item);
	g_assert_nonnull(surface);
	g_assert_cmpint(cairo_image_surface_get_width(surface), ==, 2);
	px = (guint32 *)cairo_image_surface_get_data(surface);

	/*
	 * cairo's ARGB32 is a native-endian WORD, so the test reads words
	 * rather than bytes and stays right on either endianness.
	 *
	 * Opaque red is 0xffff0000.  Read as if the wire order were cairo's
	 * byte order it comes out 0x0000ffff -- opaque CYAN, which is what
	 * every icon looks like when this is wrong: a blue-tinted negative.
	 */
	g_assert_cmphex(px[0], ==, 0xffff0000u);

	/*
	 * And half-transparent blue is PREMULTIPLIED: 0xff * 0x80 / 0xff is
	 * 0x80, so the stored blue is 0x80 and not 0xff.  Without this the
	 * colour is too strong wherever the icon is transparent, which
	 * reads as a halo around anything with a soft edge.
	 */
	g_assert_cmphex(px[1], ==, 0x80000080u);

	cairo_surface_destroy(surface);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

#define CASE(path, fn) \
	g_test_add(path, Fixture, NULL, fixture_up, fn, fixture_down)

	CASE("/tray/the-watcher-is-served", test_the_watcher_is_served);
	CASE("/tray/register-by-bus-name", test_registering_by_bus_name);
	CASE("/tray/register-by-object-path", test_registering_by_object_path);
	CASE("/tray/register-by-both", test_registering_by_both_joined);
	CASE("/tray/the-other-spelling",
	     test_the_freedesktop_spelling_is_read_too);
	CASE("/tray/pixmap-choice", test_the_middle_pixmap_is_taken);
	CASE("/tray/one-small-pixmap", test_a_lone_small_pixmap_is_still_taken);
	CASE("/tray/an-application-that-leaves",
	     test_an_application_that_leaves_takes_its_icon);
	CASE("/tray/a-click-arrives", test_a_click_reaches_the_application);
	CASE("/tray/the-menu-is-a-tree",
	     test_the_menu_is_a_tree_without_mnemonics);

#undef CASE
	g_test_add_func("/tray/a-pixmap-becomes-a-surface",
	                test_a_pixmap_is_converted_for_cairo);
	g_test_add_func("/tray/the-name-comes-free",
	                test_the_name_is_picked_up_when_it_comes_free);

	return g_test_run();
}
