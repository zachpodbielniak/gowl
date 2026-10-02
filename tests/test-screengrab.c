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
 * test-screengrab.c - Text and colours off the screen, end to end.
 *
 * The real screenshot module in a headless compositor whose desktop is
 * a known picture: black, with one rectangle of #336699.  The colour
 * picker is driven through the module's own mouse handler and must put
 * exactly #336699 on the clipboard.  OCR runs a stand-in for tesseract
 * that checks it was handed a real PNG and the documented arguments,
 * and prints a line; that line must reach the clipboard and the
 * temporary image must be gone afterwards.  Escape cancels both, and a
 * missing OCR program is reported rather than fatal.
 */

#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_scene.h>
#include <xkbcommon/xkbcommon.h>

#include "gowl.h"
#include "core/gowl-core-private.h"
#include "core/gowl-seat.h"

#define MARK_X (200)
#define MARK_Y (150)
#define MARK_W (100)
#define MARK_H (80)

typedef struct {
	gchar             *runtime;
	gchar             *script;
	gchar             *argfile;
	GowlConfig        *config;
	GowlModuleManager *modules;
	GowlCompositor    *compositor;
	GowlModule        *shot;
	gboolean           up;
} Rig;

static void
rm_rf(const gchar *path)
{
	g_autoptr(GDir) dir = g_dir_open(path, 0, NULL);
	const gchar *e;

	if (dir != NULL) {
		while ((e = g_dir_read_name(dir)) != NULL) {
			g_autofree gchar *p = g_build_filename(path, e, NULL);

			if (g_file_test(p, G_FILE_TEST_IS_DIR)
			    && !g_file_test(p, G_FILE_TEST_IS_SYMLINK))
				rm_rf(p);
			else
				g_unlink(p);
		}
	}
	g_rmdir(path);
}

static void
configure(Rig *r, const gchar *key, const gchar *value)
{
	g_autoptr(GHashTable) s = g_hash_table_new(g_str_hash, g_str_equal);

	g_hash_table_insert(s, (gpointer)key, (gpointer)value);
	gowl_module_configure(r->shot, s);
}

static void
rig_setup(Rig *r, gconstpointer data)
{
	g_autofree gchar *so = NULL;
	g_autofree gchar *body = NULL;
	GError *error = NULL;

	(void)data;
	memset(r, 0, sizeof *r);
	r->runtime = g_build_filename(g_get_tmp_dir(), "gowl-grab-XXXXXX", NULL);
	g_assert_nonnull(g_mkdtemp_full(r->runtime, 0700));
	g_setenv("GOWL_DISABLE_SYSTEMD", "1", TRUE);
	g_setenv("XDG_RUNTIME_DIR", r->runtime, TRUE);
	g_setenv("WLR_BACKENDS", "headless", TRUE);
	g_setenv("WLR_HEADLESS_OUTPUTS", "1", TRUE);
	g_setenv("WLR_RENDERER", "pixman", TRUE);

	/* The stand-in for tesseract: IMAGE stdout -l LANG, as the module
	   documents.  It records what it was given and refuses an empty
	   or missing image, as the real one would. */
	r->argfile = g_build_filename(r->runtime, "ocr-args", NULL);
	r->script = g_build_filename(r->runtime, "fake-ocr", NULL);
	body = g_strdup_printf(
		"#!/bin/sh\n"
		"printf '%%s|%%s|%%s|%%s' \"$1\" \"$2\" \"$3\" \"$4\" > '%s'\n"
		"[ -s \"$1\" ] || { echo 'no image' >&2; exit 1; }\n"
		"head -c 8 \"$1\" | grep -q PNG || { echo 'not a png' >&2; exit 1; }\n"
		"printf 'Hello from the screen\\n\\f'\n",
		r->argfile);
	g_assert_true(g_file_set_contents(r->script, body, -1, NULL));
	g_chmod(r->script, 0755);

	r->config = gowl_config_new();
	gowl_config_set_lock_command(r->config, "");
	r->modules = gowl_module_manager_new();
	so = g_build_filename(GOWL_TEST_MODULE_DIR, "screenshot.so", NULL);
	if (!gowl_module_manager_load_module(r->modules, so, &error)) {
		g_test_skip("screenshot.so did not load");
		g_clear_error(&error);
		return;
	}
	gowl_module_manager_activate_all(r->modules);
	r->shot = gowl_module_manager_find_module(r->modules, "screenshot");
	g_assert_nonnull(r->shot);

	r->compositor = gowl_compositor_new();
	gowl_compositor_set_config(r->compositor, r->config);
	gowl_compositor_set_module_manager(r->compositor, r->modules);
	if (!gowl_compositor_start(r->compositor, &error)) {
		g_test_skip("no headless compositor here");
		g_clear_error(&error);
		return;
	}
	gowl_module_manager_dispatch_startup(r->modules, r->compositor);
	configure(r, "ocr-command", r->script);
	configure(r, "save-directory", r->runtime);

	/* the desktop: black, and one #336699 rectangle */
	{
		struct wlr_scene_tree *bg;
		struct wlr_scene_rect *ground;
		struct wlr_scene_rect *mark;
		float black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
		float blue[4] = { 0x33 / 255.0f, 0x66 / 255.0f, 0x99 / 255.0f, 1.0f };
		gint mw = 0;
		gint mh = 0;

		bg = gowl_compositor_get_scene_layer(r->compositor,
		                                     GOWL_SCENE_LAYER_BG);
		g_assert_nonnull(bg);
		gowl_monitor_get_geometry(r->compositor->selmon, NULL, NULL,
		                          &mw, &mh);
		ground = wlr_scene_rect_create(bg, mw, mh, black);
		wlr_scene_node_set_position(&ground->node, 0, 0);
		mark = wlr_scene_rect_create(bg, MARK_W, MARK_H, blue);
		wlr_scene_node_set_position(&mark->node, MARK_X, MARK_Y);
	}
	r->up = TRUE;
}

static void
rig_teardown(Rig *r, gconstpointer data)
{
	(void)data;
	if (r->compositor != NULL && r->modules != NULL)
		gowl_module_manager_dispatch_shutdown(r->modules, r->compositor);
	g_clear_object(&r->compositor);
	g_clear_object(&r->modules);
	g_clear_object(&r->config);
	rm_rf(r->runtime);
	g_free(r->runtime);
	g_free(r->script);
	g_free(r->argfile);
}

#define RIG_UP(r) do { if (!(r)->up) return; } while (0)

static gchar *
cmd(Rig *r, const gchar *line)
{
	gchar *reply = gowl_compositor_run_command(r->compositor, line);

	g_assert_nonnull(reply);
	return g_strchomp(reply);
}

static gchar *
clipboard(Rig *r)
{
	return gowl_seat_get_clipboard(gowl_compositor_get_seat(r->compositor));
}

/* A left click at (@x, @y) through the module's own mouse handler. */
static void
click(Rig *r, gdouble x, gdouble y)
{
	GowlMouseHandler *m = GOWL_MOUSE_HANDLER(r->shot);

	gowl_mouse_handler_handle_motion(m, x, y);
	g_assert_true(gowl_mouse_handler_handle_button(m, 0x110, 1, 0));
	g_assert_true(gowl_mouse_handler_handle_button(m, 0x110, 0, 0));
}

/* A drag from one corner to the other. */
static void
drag(Rig *r, gdouble x0, gdouble y0, gdouble x1, gdouble y1)
{
	GowlMouseHandler *m = GOWL_MOUSE_HANDLER(r->shot);

	gowl_mouse_handler_handle_motion(m, x0, y0);
	g_assert_true(gowl_mouse_handler_handle_button(m, 0x110, 1, 0));
	gowl_mouse_handler_handle_motion(m, x1, y1);
	g_assert_true(gowl_mouse_handler_handle_button(m, 0x110, 0, 0));
}

static void
pump_until_clipboard(Rig *r, const gchar *want, guint ms)
{
	struct wl_event_loop *loop = gowl_compositor_get_event_loop(r->compositor);
	gint64 end = g_get_monotonic_time() + (gint64)ms * 1000;

	while (g_get_monotonic_time() < end) {
		g_autofree gchar *now = clipboard(r);

		if (g_strcmp0(now, want) == 0)
			return;
		wl_event_loop_dispatch(loop, 10);
		while (g_main_context_iteration(NULL, FALSE))
			;
	}
}

static void
pump(Rig *r, guint ms)
{
	struct wl_event_loop *loop = gowl_compositor_get_event_loop(r->compositor);
	gint64 end = g_get_monotonic_time() + (gint64)ms * 1000;

	while (g_get_monotonic_time() < end)
		wl_event_loop_dispatch(loop, 10);
}

/* Whether any gowl-ocr-*.png is left in the runtime directory. */
static gboolean
ocr_image_left(Rig *r)
{
	g_autoptr(GDir) dir = g_dir_open(r->runtime, 0, NULL);
	const gchar *e;

	while (dir != NULL && (e = g_dir_read_name(dir)) != NULL)
		if (g_str_has_prefix(e, "gowl-ocr-"))
			return TRUE;
	return FALSE;
}

static void
test_color_pick(Rig *r, gconstpointer data)
{
	g_autofree gchar *armed = NULL;
	g_autofree gchar *busy = NULL;
	g_autofree gchar *got = NULL;
	g_autofree gchar *black = NULL;

	(void)data;
	RIG_UP(r);
	armed = cmd(r, "screenshot-color");
	g_assert_cmpstr(armed, ==, "OK click a pixel, Escape to cancel");
	busy = cmd(r, "screenshot-ocr");
	g_assert_cmpstr(busy, ==, "ERROR a selection is already in progress");

	click(r, MARK_X + 20, MARK_Y + 20);
	got = clipboard(r);
	if (got == NULL) {
		g_test_skip("no capture from this renderer");
		return;
	}
	g_assert_cmpstr(got, ==, "#336699");

	/* the frame drawn while armed is gone before the sample: a pick
	   on the edge of the screen reads the desktop, not the frame */
	g_free(cmd(r, "screenshot-color"));
	click(r, 1, 1);
	black = clipboard(r);
	g_assert_cmpstr(black, ==, "#000000");
}

static void
test_ocr(Rig *r, gconstpointer data)
{
	g_autofree gchar *armed = NULL;
	g_autofree gchar *got = NULL;
	g_autofree gchar *args = NULL;
	g_auto(GStrv) parts = NULL;

	(void)data;
	RIG_UP(r);
	armed = cmd(r, "screenshot-ocr");
	g_assert_cmpstr(armed, ==, "OK drag over the text, Escape to cancel");
	drag(r, MARK_X - 10, MARK_Y - 10, MARK_X + MARK_W + 10,
	     MARK_Y + MARK_H + 10);

	pump_until_clipboard(r, "Hello from the screen", 5000);
	got = clipboard(r);
	if (got == NULL && !g_file_test(r->argfile, G_FILE_TEST_EXISTS)) {
		g_test_skip("no capture from this renderer");
		return;
	}
	/* the trailing form feed and newline are trimmed */
	g_assert_cmpstr(got, ==, "Hello from the screen");

	g_assert_true(g_file_get_contents(r->argfile, &args, NULL, NULL));
	parts = g_strsplit(args, "|", -1);
	g_assert_cmpuint(g_strv_length(parts), ==, 4);
	g_assert_true(g_str_has_suffix(parts[0], ".png"));
	g_assert_cmpstr(parts[1], ==, "stdout");
	g_assert_cmpstr(parts[2], ==, "-l");
	g_assert_cmpstr(parts[3], ==, "eng");
	/* private, and cleaned up */
	g_assert_true(g_str_has_prefix(parts[0], r->runtime));
	g_assert_false(ocr_image_left(r));

	/* the language is a setting */
	configure(r, "ocr-language", "eng+deu");
	g_free(cmd(r, "screenshot-ocr"));
	drag(r, MARK_X, MARK_Y, MARK_X + 50, MARK_Y + 50);
	pump(r, 1500);
	g_clear_pointer(&args, g_free);
	g_assert_true(g_file_get_contents(r->argfile, &args, NULL, NULL));
	g_assert_true(g_str_has_suffix(args, "|eng+deu"));
}

/* Escape cancels both; nothing is run, nothing is saved. */
static void
test_cancel(Rig *r, gconstpointer data)
{
	GowlKeybindHandler *kb;
	g_autofree gchar *again = NULL;

	(void)data;
	RIG_UP(r);
	kb = GOWL_KEYBIND_HANDLER(r->shot);

	g_free(cmd(r, "screenshot-ocr"));
	g_assert_true(gowl_keybind_handler_handle_key(kb, 0, XKB_KEY_Escape,
	                                              TRUE));
	/* no longer selecting: keys pass through again */
	g_assert_false(gowl_keybind_handler_handle_key(kb, 0, XKB_KEY_a, TRUE));
	pump(r, 300);
	g_assert_false(g_file_test(r->argfile, G_FILE_TEST_EXISTS));

	g_free(cmd(r, "screenshot-color"));
	g_assert_true(gowl_keybind_handler_handle_key(kb, 0, XKB_KEY_Escape,
	                                              TRUE));
	g_assert_false(gowl_keybind_handler_handle_key(kb, 0, XKB_KEY_a, TRUE));
	/* and both can be armed again */
	again = cmd(r, "screenshot-color");
	g_assert_true(g_str_has_prefix(again, "OK"));
	g_assert_true(gowl_keybind_handler_handle_key(kb, 0, XKB_KEY_Escape,
	                                              TRUE));
}

/* No tesseract: reported, not fatal, and no image left behind. */
static void
test_ocr_missing(Rig *r, gconstpointer data)
{
	g_autofree gchar *again = NULL;

	(void)data;
	RIG_UP(r);
	configure(r, "ocr-command", "gowl-no-such-ocr-program");
	g_free(cmd(r, "screenshot-ocr"));
	drag(r, MARK_X, MARK_Y, MARK_X + 50, MARK_Y + 50);
	pump(r, 300);
	g_assert_false(ocr_image_left(r));
	again = cmd(r, "screenshot-ocr");
	g_assert_true(g_str_has_prefix(again, "OK"));
}

int
main(
	int    argc,
	char **argv
){
	g_test_init(&argc, &argv, NULL);
#define ADD(path, fn) \
	g_test_add("/screengrab/" path, Rig, NULL, rig_setup, fn, rig_teardown)
	ADD("color-pick", test_color_pick);
	ADD("ocr", test_ocr);
	ADD("cancel", test_cancel);
	ADD("ocr-missing", test_ocr_missing);
#undef ADD
	return g_test_run();
}
