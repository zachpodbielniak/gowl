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

#include "module/gowl-module.h"
#include "module/gowl-module-manager.h"
#include "module/gowl-module-info.h"
#include "interfaces/gowl-startup-handler.h"

/* ---- Test module implementation ---- */

#define TEST_TYPE_MODULE (test_module_get_type())
G_DECLARE_FINAL_TYPE(TestModule, test_module, TEST, MODULE, GowlModule)

struct _TestModule {
	GowlModule parent_instance;
	gboolean activated;
	gboolean startup_called;
};

/* forward declarations for G_DEFINE_TYPE_WITH_CODE */
static void test_module_class_init(TestModuleClass *klass);
static void test_module_init(TestModule *self);
static void test_module_startup_handler_init(GowlStartupHandlerInterface *iface);

G_DEFINE_TYPE_WITH_CODE(TestModule, test_module, GOWL_TYPE_MODULE,
	G_IMPLEMENT_INTERFACE(GOWL_TYPE_STARTUP_HANDLER,
		test_module_startup_handler_init))

static gboolean
test_module_activate(GowlModule *mod)
{
	TestModule *self = TEST_MODULE(mod);

	self->activated = TRUE;
	return TRUE;
}

static void
test_module_deactivate(GowlModule *mod)
{
	TestModule *self = TEST_MODULE(mod);

	self->activated = FALSE;
}

static const gchar *
test_module_get_name(GowlModule *mod)
{
	(void)mod;
	return "test-module";
}

static const gchar *
test_module_get_description(GowlModule *mod)
{
	(void)mod;
	return "A test module for unit testing";
}

static const gchar *
test_module_get_version(GowlModule *mod)
{
	(void)mod;
	return "0.1.0";
}

static void
test_module_on_startup(GowlStartupHandler *handler, gpointer compositor)
{
	TestModule *self = TEST_MODULE(handler);

	(void)compositor;
	self->startup_called = TRUE;
}

static void
test_module_startup_handler_init(GowlStartupHandlerInterface *iface)
{
	iface->on_startup = test_module_on_startup;
}

static void
test_module_class_init(TestModuleClass *klass)
{
	GowlModuleClass *mod_class = GOWL_MODULE_CLASS(klass);

	mod_class->activate = test_module_activate;
	mod_class->deactivate = test_module_deactivate;
	mod_class->get_name = test_module_get_name;
	mod_class->get_description = test_module_get_description;
	mod_class->get_version = test_module_get_version;
}

static void
test_module_init(TestModule *self)
{
	self->activated = FALSE;
	self->startup_called = FALSE;
}

/* ---- Module Info tests ---- */

static void
test_module_info_new(void)
{
	GowlModuleInfo *info;

	info = gowl_module_info_new("test", "A test", "1.0");
	g_assert_nonnull(info);
	g_assert_cmpstr(gowl_module_info_get_name(info), ==, "test");
	g_assert_cmpstr(gowl_module_info_get_description(info), ==, "A test");
	g_assert_cmpstr(gowl_module_info_get_version(info), ==, "1.0");
	gowl_module_info_free(info);
}

static void
test_module_info_copy(void)
{
	GowlModuleInfo *info;
	GowlModuleInfo *copy;

	info = gowl_module_info_new("test", "desc", "1.0");
	copy = gowl_module_info_copy(info);
	g_assert_nonnull(copy);
	g_assert_cmpstr(gowl_module_info_get_name(copy), ==, "test");
	gowl_module_info_free(info);
	gowl_module_info_free(copy);
}

static void
test_module_info_type(void)
{
	GType type;

	type = gowl_module_info_get_type();
	g_assert_true(type != G_TYPE_INVALID);
	g_assert_true(G_TYPE_IS_BOXED(type));
}

/* ---- Module base class tests ---- */

static void
test_module_abstract_type(void)
{
	GType type;

	type = GOWL_TYPE_MODULE;
	g_assert_true(type != G_TYPE_INVALID);
	g_assert_true(G_TYPE_IS_ABSTRACT(type));
}

static void
test_module_subclass(void)
{
	TestModule *mod;

	mod = g_object_new(TEST_TYPE_MODULE, NULL);
	g_assert_nonnull(mod);
	g_assert_true(GOWL_IS_MODULE(mod));

	g_assert_cmpstr(gowl_module_get_name(GOWL_MODULE(mod)), ==, "test-module");
	g_assert_cmpstr(gowl_module_get_description(GOWL_MODULE(mod)), ==,
		"A test module for unit testing");

	g_object_unref(mod);
}

static void
test_module_activate_deactivate(void)
{
	TestModule *mod;

	mod = g_object_new(TEST_TYPE_MODULE, NULL);
	g_assert_false(gowl_module_get_is_active(GOWL_MODULE(mod)));
	g_assert_false(mod->activated);

	gowl_module_activate(GOWL_MODULE(mod));
	g_assert_true(gowl_module_get_is_active(GOWL_MODULE(mod)));
	g_assert_true(mod->activated);

	gowl_module_deactivate(GOWL_MODULE(mod));
	g_assert_false(gowl_module_get_is_active(GOWL_MODULE(mod)));
	g_assert_false(mod->activated);

	g_object_unref(mod);
}

static void
test_module_priority(void)
{
	TestModule *mod;

	mod = g_object_new(TEST_TYPE_MODULE, "priority", -100, NULL);
	g_assert_cmpint(gowl_module_get_priority(GOWL_MODULE(mod)), ==, -100);

	gowl_module_set_priority(GOWL_MODULE(mod), 50);
	g_assert_cmpint(gowl_module_get_priority(GOWL_MODULE(mod)), ==, 50);

	g_object_unref(mod);
}

/* ---- Module Manager tests ---- */

static void
test_manager_new(void)
{
	GowlModuleManager *mgr;

	mgr = gowl_module_manager_new();
	g_assert_nonnull(mgr);
	g_assert_true(GOWL_IS_MODULE_MANAGER(mgr));

	g_object_unref(mgr);
}

static void
test_manager_register(void)
{
	GowlModuleManager *mgr;
	gboolean ok;
	GError *error = NULL;

	mgr = gowl_module_manager_new();

	ok = gowl_module_manager_register(mgr, TEST_TYPE_MODULE, &error);
	g_assert_true(ok);
	g_assert_no_error(error);

	g_object_unref(mgr);
}

static void
test_manager_activate_all(void)
{
	GowlModuleManager *mgr;

	mgr = gowl_module_manager_new();
	gowl_module_manager_register(mgr, TEST_TYPE_MODULE, NULL);
	gowl_module_manager_activate_all(mgr);

	/* Module should now be active */
	/* (verified by the module's own activate callback) */

	gowl_module_manager_deactivate_all(mgr);
	g_object_unref(mgr);
}

static void
test_manager_dispatch_startup(void)
{
	GowlModuleManager *mgr;

	mgr = gowl_module_manager_new();
	gowl_module_manager_register(mgr, TEST_TYPE_MODULE, NULL);
	gowl_module_manager_activate_all(mgr);

	/* Dispatch startup - our test module implements GowlStartupHandler */
	gowl_module_manager_dispatch_startup(mgr, NULL);

	gowl_module_manager_deactivate_all(mgr);
	g_object_unref(mgr);
}

int
main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);

	/* Module Info */
	g_test_add_func("/module/info/new", test_module_info_new);
	g_test_add_func("/module/info/copy", test_module_info_copy);
	g_test_add_func("/module/info/type", test_module_info_type);

	/* Module base class */
	g_test_add_func("/module/abstract-type", test_module_abstract_type);
	g_test_add_func("/module/subclass", test_module_subclass);
	g_test_add_func("/module/activate-deactivate", test_module_activate_deactivate);
	g_test_add_func("/module/priority", test_module_priority);

	/* Module Manager */
	g_test_add_func("/module/manager/new", test_manager_new);
	g_test_add_func("/module/manager/register", test_manager_register);
	g_test_add_func("/module/manager/activate-all", test_manager_activate_all);
	g_test_add_func("/module/manager/dispatch-startup", test_manager_dispatch_startup);

	return g_test_run();
}
