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
 * The `gabbrev' widget, driven with the module's own replies.
 *
 * Asserted:
 *
 *   THE BAR SAYS WHAT STATE IT IS IN.  Not loaded and off are muted;
 *   expansions still resolving are counted and coloured; idle is plain.
 *
 *   THE PANEL OFFERS WHAT CAN BE DONE.  A resolving expansion is a row
 *   whose id cancels that job; a suggestion is a row that accepts; the
 *   most used matches and the never-used count are listed.
 *
 *   `x' DISMISSES THE FOCUSED SUGGESTION and nothing else; other rows
 *   leave the key to the host.
 *
 *   A HOSTILE REPLY IS "NOT LOADED", never a crash: not JSON, the wrong
 *   shape, members of the wrong type.
 */

#include <glib.h>
#include <string.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#ifndef GOWL_TEST_BAR_MODULE
#define GOWL_TEST_BAR_MODULE "build/release/modules/bar.so"
#endif

#define BAR_HARNESS_MODULE GOWL_TEST_BAR_MODULE
#include "bar-widget-harness.h"
#include "barkit/gowl-bar-plugin-proxy.h"

typedef void (*ApplyFunc) (GowlBarPlugin *plugin, gpointer data,
                           const gchar *reply);

#define IDLE \
	"OK {\"active\":true,\"on\":true,\"matches\":51,\"jobs\":[]," \
	"\"stats\":{\"on\":true,\"tracked\":2,\"top\":[" \
	"{\"id\":\";today\",\"uses\":7,\"last-used\":1760000000}," \
	"{\"id\":\";shrug\",\"uses\":2,\"last-used\":1760000001}]," \
	"\"never-used\":12}," \
	"\"learning\":{\"on\":true,\"phrases\":40,\"suggestions\":1}}"

#define BUSY \
	"OK {\"active\":true,\"on\":true,\"matches\":51,\"jobs\":[" \
	"{\"id\":4,\"match\":\";weather\",\"state\":\"resolving\"," \
	"\"waiting-for\":\"shell\",\"seconds\":2.5}]," \
	"\"stats\":{\"top\":[],\"never-used\":0},\"learning\":{\"on\":false}}"

#define OFF \
	"OK {\"active\":true,\"on\":false,\"matches\":3,\"jobs\":[]}"

#define SUGGESTIONS \
	"OK [{\"phrase\":\"thank you very much\",\"trigger\":\";tyvm\"," \
	"\"count\":3,\"words\":4,\"last\":1760000000}," \
	"{\"phrase\":\"see you soon\",\"trigger\":null,\"count\":5}]"

typedef struct {
	BarHarness  h;
	gpointer    data;
	ApplyFunc   status;
	ApplyFunc   suggestions;
} Widget;

static gboolean
widget_up(Widget *w)
{
	if (!bar_harness_up(&w->h, "bar_register_gabbrev_plugins", "gabbrev"))
		return FALSE;
	w->status = (ApplyFunc)bar_harness_sym(&w->h, "bar_gabbrev_apply_status");
	w->suggestions = (ApplyFunc)bar_harness_sym(&w->h,
	                                            "bar_gabbrev_apply_suggestions");
	g_assert_nonnull(w->status);
	g_assert_nonnull(w->suggestions);
	bar_harness_activate(&w->h);
	w->data = gowl_bar_plugin_proxy_get_data(GOWL_BAR_PLUGIN_PROXY(w->h.plugin));
	g_assert_nonnull(w->data);
	return TRUE;
}

/* The index of the panel item with @id, or -1 */
static gint
item_index(
	GowlBarPanel *panel,
	const gchar  *id
){
	guint i;

	for (i = 0; i < gowl_bar_panel_n_items(panel); i++)
		if (g_strcmp0(gowl_bar_panel_item_get_id(
			gowl_bar_panel_get_item(panel, i)), id) == 0)
			return (gint)i;
	return -1;
}

/* Not loaded: muted, and the panel says so */
static void
test_unavailable(void)
{
	Widget w;
	g_autoptr(GowlBarPanel) panel = NULL;
	g_autofree gchar *tip = NULL;

	if (!widget_up(&w))
		return;
	w.status(w.h.plugin, w.data, NULL);
	g_assert_cmpint(gowl_bar_plugin_get_color(w.h.plugin), ==,
	                GOWL_BAR_COLOR_MUTED);
	tip = gowl_bar_plugin_dup_tooltip(w.h.plugin);
	g_assert_nonnull(strstr(tip, "not loaded"));
	panel = bar_harness_panel(&w.h);
	g_assert_true(bar_harness_panel_mentions(panel, "not loaded"));
	bar_harness_down(&w.h);
}

/* On and idle: plain, with usage in the panel */
static void
test_idle(void)
{
	Widget w;
	g_autoptr(GowlBarPanel) panel = NULL;
	g_autofree gchar *label = NULL;
	g_autofree gchar *tip = NULL;

	if (!widget_up(&w))
		return;
	w.status(w.h.plugin, w.data, IDLE);
	label = gowl_bar_plugin_dup_label(w.h.plugin);
	g_assert_true(label == NULL || *label == '\0');
	g_assert_cmpint(gowl_bar_plugin_get_color(w.h.plugin), ==,
	                GOWL_BAR_COLOR_TEXT);
	tip = gowl_bar_plugin_dup_tooltip(w.h.plugin);
	g_assert_nonnull(strstr(tip, "51 match"));
	g_assert_nonnull(strstr(tip, "phrases to accept"));

	panel = bar_harness_panel(&w.h);
	g_assert_true(bar_harness_panel_mentions(panel, "Turn off"));
	g_assert_true(bar_harness_panel_mentions(panel, ";today"));
	g_assert_true(bar_harness_panel_mentions(panel, "7 uses"));
	g_assert_true(bar_harness_panel_mentions(panel, "Never used"));
	g_assert_true(bar_harness_panel_mentions(panel, "12 match"));
	g_assert_cmpint(item_index(panel, "cancel-all"), ==, -1);
	bar_harness_down(&w.h);
}

/* Off: muted, and the panel offers to turn it on */
static void
test_off(void)
{
	Widget w;
	g_autoptr(GowlBarPanel) panel = NULL;

	if (!widget_up(&w))
		return;
	w.status(w.h.plugin, w.data, OFF);
	g_assert_cmpint(gowl_bar_plugin_get_color(w.h.plugin), ==,
	                GOWL_BAR_COLOR_MUTED);
	panel = bar_harness_panel(&w.h);
	g_assert_true(bar_harness_panel_mentions(panel, "Turn on"));
	bar_harness_down(&w.h);
}

/* A resolving expansion: counted, and a row that names its job */
static void
test_busy(void)
{
	Widget w;
	g_autoptr(GowlBarPanel) panel = NULL;
	g_autofree gchar *label = NULL;

	if (!widget_up(&w))
		return;
	w.status(w.h.plugin, w.data, BUSY);
	label = gowl_bar_plugin_dup_label(w.h.plugin);
	g_assert_cmpstr(label, ==, "1");
	g_assert_cmpint(gowl_bar_plugin_get_color(w.h.plugin), ==,
	                GOWL_BAR_COLOR_YELLOW);
	panel = bar_harness_panel(&w.h);
	g_assert_cmpint(item_index(panel, "job:4"), >=, 0);
	g_assert_cmpint(item_index(panel, "cancel-all"), >=, 0);
	g_assert_true(bar_harness_panel_mentions(panel, ";weather"));
	g_assert_true(bar_harness_panel_mentions(panel, "waiting for shell"));
	bar_harness_down(&w.h);
}

/* Suggestions: rows to accept; `x' dismisses only a suggestion */
static void
test_suggestions(void)
{
	Widget w;
	g_autoptr(GowlBarPanel) panel = NULL;
	g_autoptr(GowlBarPanel) after = NULL;
	gint sug, other;

	if (!widget_up(&w))
		return;
	w.status(w.h.plugin, w.data, IDLE);
	w.suggestions(w.h.plugin, w.data, SUGGESTIONS);
	panel = bar_harness_panel(&w.h);
	sug = item_index(panel, "sug:0");
	g_assert_cmpint(sug, >=, 0);
	g_assert_true(bar_harness_panel_mentions(panel, "thank you very much"));
	g_assert_true(bar_harness_panel_mentions(panel, ";tyvm"));
	/* No trigger was free for the second: it says so */
	g_assert_true(bar_harness_panel_mentions(panel, "no free trigger"));

	/* A key on a row that is not a suggestion is the host's */
	other = item_index(panel, "act");
	g_assert_cmpint(other, >=, 0);
	g_assert_false(gowl_bar_plugin_panel_key(w.h.plugin, XKB_KEY_x, 0,
	                                         other));
	g_assert_false(gowl_bar_plugin_panel_key(w.h.plugin, XKB_KEY_q, 0, sug));
	g_assert_true(gowl_bar_plugin_panel_key(w.h.plugin, XKB_KEY_x, 0, sug));
	after = bar_harness_panel(&w.h);
	g_assert_false(bar_harness_panel_mentions(after, "thank you very much"));
	g_assert_true(bar_harness_panel_mentions(after, "see you soon"));
	bar_harness_down(&w.h);
}

/* Hostile replies: never a crash, never a half-read state */
static void
test_hostile(void)
{
	static const gchar * const replies[] = {
		"ERROR unknown command gabbrev-status",
		"OK not json",
		"OK [1,2,3]",
		"OK {\"on\":\"yes\",\"matches\":\"many\",\"jobs\":{\"id\":1},"
		"\"stats\":[],\"learning\":7}",
		"OK {\"jobs\":[1,null,\"x\",{\"id\":\"seven\"}]}",
		"",
		NULL
	};
	Widget w;
	guint i;

	if (!widget_up(&w))
		return;
	for (i = 0; replies[i] != NULL; i++) {
		g_autoptr(GowlBarPanel) panel = NULL;

		w.status(w.h.plugin, w.data, replies[i]);
		w.suggestions(w.h.plugin, w.data, replies[i]);
		panel = bar_harness_panel(&w.h);
		g_assert_nonnull(panel);
	}
	/* Not JSON is "not loaded" */
	w.status(w.h.plugin, w.data, "OK not json");
	g_assert_cmpint(gowl_bar_plugin_get_color(w.h.plugin), ==,
	                GOWL_BAR_COLOR_MUTED);
	bar_harness_down(&w.h);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/bar/gabbrev/unavailable", test_unavailable);
	g_test_add_func("/bar/gabbrev/idle", test_idle);
	g_test_add_func("/bar/gabbrev/off", test_off);
	g_test_add_func("/bar/gabbrev/busy", test_busy);
	g_test_add_func("/bar/gabbrev/suggestions", test_suggestions);
	g_test_add_func("/bar/gabbrev/hostile", test_hostile);
	return g_test_run();
}
