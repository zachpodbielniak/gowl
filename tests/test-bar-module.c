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
 * The bar module's configuration, against the real module.
 *
 * The module is loaded and configured without a compositor -- it only
 * touches the scene graph once one is attached -- so what a given
 * configuration actually produces can be asserted here rather than by
 * logging in and counting widgets.  Which is how the duplicate clock
 * got shipped: every piece was individually right.
 */

#include <gmodule.h>
#include <string.h>

#include "module/gowl-module.h"
#include "interfaces/gowl-ipc-handler.h"

#ifndef GOWL_TEST_BAR_MODULE
#define GOWL_TEST_BAR_MODULE "build/release/modules/bar.so"
#endif

static GowlModule *
load_bar_module(void)
{
	static GType type = 0;
	GModule *module;
	gpointer symbol;
	GType (*register_fn)(void);

	if (type == 0) {
		module = g_module_open(GOWL_TEST_BAR_MODULE,
		                       G_MODULE_BIND_LAZY);
		if (module == NULL) {
			g_test_skip(g_module_error());
			return NULL;
		}
		g_module_make_resident(module);
		if (!g_module_symbol(module, "gowl_module_register", &symbol)) {
			g_test_skip("bar.so exports no gowl_module_register");
			return NULL;
		}
		register_fn = (GType (*)(void))symbol;
		type = register_fn();
	}
	return (GowlModule *)g_object_new(type, NULL);
}

static GHashTable *
settings_new(const gchar *first_key, ...)
{
	GHashTable *table;
	va_list args;
	const gchar *key;

	table = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
	                              g_free);
	if (first_key == NULL)
		return table;

	va_start(args, first_key);
	key = first_key;
	while (key != NULL) {
		const gchar *value = va_arg(args, const gchar *);

		g_hash_table_insert(table, g_strdup(key), g_strdup(value));
		key = va_arg(args, const gchar *);
	}
	va_end(args);
	return table;
}

/* Count how many times a widget name appears in the module's layout
   listing, across both bars. */
static gint
count_widget(const gchar *listing, const gchar *spec)
{
	g_auto(GStrv) lines = NULL;
	gint i, found;

	if (listing == NULL)
		return 0;

	found = 0;
	lines = g_strsplit(listing, "\n", -1);
	for (i = 0; lines[i] != NULL; i++) {
		g_auto(GStrv) fields = NULL;

		if (lines[i][0] != ' ')
			continue;    /* a slot heading, not a widget */
		fields = g_strsplit_set(lines[i], " \t", -1);
		{
			gint f;

			for (f = 0; fields[f] != NULL; f++) {
				if (g_strcmp0(fields[f], spec) == 0) {
					found++;
					break;
				}
			}
		}
	}
	return found;
}

static gchar *
layout_of(GowlModule *module)
{
	return gowl_ipc_handler_handle_command(GOWL_IPC_HANDLER(module),
	                                       "bar-widgets", NULL);
}

static void
test_the_shipped_layout(void)
{
	GowlModule *module;
	g_autofree gchar *listing = NULL;

	module = load_bar_module();
	if (module == NULL)
		return;

	/* Nobody has configured anything: the shipped layout stands. */
	listing = layout_of(module);
	g_assert_nonnull(listing);
	g_assert_cmpint(count_widget(listing, "clock"), ==, 1);
	g_assert_cmpint(count_widget(listing, "tags"), ==, 1);
	g_assert_cmpint(count_widget(listing, "title"), ==, 1);
	g_assert_cmpint(count_widget(listing, "cpu"), ==, 1);

	g_object_unref(module);
}

static void
test_a_pre_regions_config_gets_one_clock(void)
{
	GowlModule *module;
	g_autoptr(GHashTable) settings = NULL;
	g_autofree gchar *listing = NULL;

	module = load_bar_module();
	if (module == NULL)
		return;

	/*
	 * The exact configuration that shipped a duplicate clock: a
	 * pre-regions `widgets' list ending in one, applied on top of a
	 * shipped layout that already had a clock in the centre.
	 */
	settings = settings_new("widgets",
	                        "cpu memory disk:/var battery clock", NULL);
	gowl_module_configure(module, settings);

	listing = layout_of(module);
	g_assert_nonnull(listing);
	g_assert_cmpint(count_widget(listing, "clock"), ==, 1);

	/* And the bar it was written for drew the tag row and the window
	   title unconditionally, so clearing the shipped layout must not
	   cost it those. */
	g_assert_cmpint(count_widget(listing, "tags"), ==, 1);
	g_assert_cmpint(count_widget(listing, "title"), ==, 1);

	/* Its own list survives intact, parameters included. */
	g_assert_cmpint(count_widget(listing, "disk:/var"), ==, 1);
	g_assert_cmpint(count_widget(listing, "battery"), ==, 1);

	g_object_unref(module);
}

static void
test_a_regions_config_replaces_the_shipped_layout(void)
{
	GowlModule *module;
	g_autoptr(GHashTable) settings = NULL;
	g_autofree gchar *listing = NULL;

	module = load_bar_module();
	if (module == NULL)
		return;

	/* A config that names only the right region still drops the
	   shipped centre clock: the shipped layout is a starting point,
	   not a base to add to. */
	settings = settings_new("widgets-right", "cpu memory", NULL);
	gowl_module_configure(module, settings);

	listing = layout_of(module);
	g_assert_cmpint(count_widget(listing, "clock"), ==, 0);
	g_assert_cmpint(count_widget(listing, "tags"), ==, 0);
	g_assert_cmpint(count_widget(listing, "cpu"), ==, 1);

	g_object_unref(module);
}

static void
test_later_configures_are_incremental(void)
{
	GowlModule *module;
	g_autoptr(GHashTable) first = NULL;
	g_autoptr(GHashTable) second = NULL;
	g_autofree gchar *listing = NULL;

	module = load_bar_module();
	if (module == NULL)
		return;

	first = settings_new("widgets-left", "tags title",
	                     "widgets-center", "clock",
	                     "widgets-right", "cpu", NULL);
	gowl_module_configure(module, first);

	/* Once the shipped layout is gone, a call naming one region must
	   leave the others alone -- otherwise configuring the bar in two
	   steps would be impossible. */
	second = settings_new("widgets-right", "memory battery", NULL);
	gowl_module_configure(module, second);

	listing = layout_of(module);
	g_assert_cmpint(count_widget(listing, "tags"), ==, 1);
	g_assert_cmpint(count_widget(listing, "clock"), ==, 1);
	g_assert_cmpint(count_widget(listing, "cpu"), ==, 0);
	g_assert_cmpint(count_widget(listing, "memory"), ==, 1);
	g_assert_cmpint(count_widget(listing, "battery"), ==, 1);

	g_object_unref(module);
}

static void
test_the_bottom_bar_is_addressed_by_prefix(void)
{
	GowlModule *module;
	g_autoptr(GHashTable) settings = NULL;
	g_autofree gchar *listing = NULL;
	const gchar *bottom;

	module = load_bar_module();
	if (module == NULL)
		return;

	/* One module owns both slots and a YAML block names one module,
	   so the bottom bar's keys are the same keys prefixed. */
	settings = settings_new("widgets-right", "cpu",
	                        "bottom-widgets-right", "ip podman", NULL);
	gowl_module_configure(module, settings);

	listing = layout_of(module);
	g_assert_nonnull(listing);

	bottom = strstr(listing, "bottom\t");
	g_assert_nonnull(bottom);
	g_assert_nonnull(strstr(bottom, "ip"));
	g_assert_nonnull(strstr(bottom, "podman"));
	/* And the prefixed keys must not have reached the top slot. */
	g_assert_cmpint(count_widget(listing, "ip"), ==, 1);

	g_object_unref(module);
}

static void
test_position_bottom_reaches_the_bottom_slot(void)
{
	GowlModule *module;
	g_autoptr(GHashTable) settings = NULL;
	g_autofree gchar *listing = NULL;
	const gchar *bottom;

	module = load_bar_module();
	if (module == NULL)
		return;

	/* The route a configuration pushed from Elisp takes. */
	settings = settings_new("position", "bottom",
	                        "widgets-right", "ip", NULL);
	gowl_module_configure(module, settings);

	listing = layout_of(module);
	bottom = strstr(listing, "bottom\t");
	g_assert_nonnull(bottom);
	g_assert_nonnull(strstr(bottom, "ip"));

	g_object_unref(module);
}

static void
test_unknown_widgets_are_skipped_not_fatal(void)
{
	GowlModule *module;
	g_autoptr(GHashTable) settings = NULL;
	g_autofree gchar *listing = NULL;
	GLogLevelFlags saved;

	module = load_bar_module();
	if (module == NULL)
		return;

	/* A typo in a widget list costs that widget, never the bar. */
	saved = g_log_set_always_fatal(G_LOG_LEVEL_ERROR);
	settings = settings_new("widgets-right", "cpu notawidget memory",
	                        NULL);
	gowl_module_configure(module, settings);
	g_log_set_always_fatal(saved);

	listing = layout_of(module);
	g_assert_cmpint(count_widget(listing, "cpu"), ==, 1);
	g_assert_cmpint(count_widget(listing, "memory"), ==, 1);
	g_assert_cmpint(count_widget(listing, "notawidget"), ==, 0);

	g_object_unref(module);
}

static void
test_aliases_and_specs(void)
{
	GowlModule *module;
	g_autoptr(GHashTable) settings = NULL;
	g_autofree gchar *listing = NULL;

	module = load_bar_module();
	if (module == NULL)
		return;

	/* Old names keep working, and a spec stays whole so two
	   instances of one widget remain separately addressable. */
	settings = settings_new("widgets-right",
	                        "mem bat disk:/var disk:/home cmd:pomo@5",
	                        NULL);
	gowl_module_configure(module, settings);

	listing = layout_of(module);
	g_assert_cmpint(count_widget(listing, "mem"), ==, 1);
	g_assert_cmpint(count_widget(listing, "bat"), ==, 1);
	g_assert_cmpint(count_widget(listing, "disk:/var"), ==, 1);
	g_assert_cmpint(count_widget(listing, "disk:/home"), ==, 1);
	g_assert_cmpint(count_widget(listing, "cmd:pomo@5"), ==, 1);

	g_object_unref(module);
}

int
main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/bar-module/shipped-layout", test_the_shipped_layout);
	g_test_add_func("/bar-module/pre-regions-one-clock",
	                test_a_pre_regions_config_gets_one_clock);
	g_test_add_func("/bar-module/regions-replace-shipped",
	                test_a_regions_config_replaces_the_shipped_layout);
	g_test_add_func("/bar-module/later-configures-incremental",
	                test_later_configures_are_incremental);
	g_test_add_func("/bar-module/bottom-prefix",
	                test_the_bottom_bar_is_addressed_by_prefix);
	g_test_add_func("/bar-module/position-bottom",
	                test_position_bottom_reaches_the_bottom_slot);
	g_test_add_func("/bar-module/unknown-widget-skipped",
	                test_unknown_widgets_are_skipped_not_fatal);
	g_test_add_func("/bar-module/aliases-and-specs",
	                test_aliases_and_specs);

	return g_test_run();
}
