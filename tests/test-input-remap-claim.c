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
 * test-input-remap-claim.c - Per-device remapping in a real compositor.
 *
 * A headless compositor with the real inputremap module loaded, and
 * software keyboards and pointers plugged into its backend the way
 * libinput plugs real ones in.  Keys are pressed on the device itself
 * (wlr_keyboard_notify_key), so they take exactly the path hardware
 * does: through the keyboard group for an unclaimed device, through the
 * remap core for a claimed one.  What comes out is watched where the
 * compositor's pipeline ends -- the embedder key intercept, the custom
 * action handler, and a mouse-handler module for buttons.
 *
 * Covers: no module => unchanged; claim on hotplug; claim and release
 * on rule add/remove; the action / key / button / drop / pass outputs;
 * a release that mirrors its press across a rule change; Super+Escape
 * beating every rule; a device destroyed while it holds a key; pointer
 * buttons and wheel notches on a claimed mouse.
 */

#include <glib.h>
#include <glib/gstdio.h>
#include <linux/input-event-codes.h>
#include <string.h>
#include <wayland-server-core.h>
#include <wlr/interfaces/wlr_keyboard.h>
#include <wlr/interfaces/wlr_pointer.h>

#include "gowl.h"
#include "core/gowl-core-private.h"

/* ── A probe module: sees every button the pipeline delivers ─────── */

#define TEST_TYPE_PROBE (test_probe_get_type())
G_DECLARE_FINAL_TYPE(TestProbe, test_probe, TEST, PROBE, GowlModule)

struct _TestProbe {
	GowlModule parent_instance;
};

typedef struct {
	guint32  code;
	gboolean pressed;
	guint32  mods;
} Seen;

/* What came out, in order.  Globals: the probe and the intercept are
 * plain callbacks with nowhere else to put it. */
static GArray    *seen_keys;
static GArray    *seen_buttons;
static GPtrArray *seen_actions;

static gboolean
probe_handle_button(
	GowlMouseHandler *handler,
	guint             button,
	guint             state,
	guint             modifiers
){
	Seen s;

	(void)handler;
	s.code = button;
	s.pressed = state != 0;
	s.mods = modifiers;
	g_array_append_val(seen_buttons, s);
	return FALSE;
}

static void
probe_mouse_init(GowlMouseHandlerInterface *iface)
{
	iface->handle_button = probe_handle_button;
}

G_DEFINE_TYPE_WITH_CODE(TestProbe, test_probe, GOWL_TYPE_MODULE,
	G_IMPLEMENT_INTERFACE(GOWL_TYPE_MOUSE_HANDLER, probe_mouse_init))

static gboolean probe_activate(GowlModule *m) { (void)m; return TRUE; }
static const gchar *probe_name(GowlModule *m) { (void)m; return "test-probe"; }
static const gchar *probe_desc(GowlModule *m) { (void)m; return "probe"; }
static const gchar *probe_version(GowlModule *m) { (void)m; return "0"; }

static void
test_probe_class_init(TestProbeClass *klass)
{
	GowlModuleClass *mc = GOWL_MODULE_CLASS(klass);

	mc->activate = probe_activate;
	mc->get_name = probe_name;
	mc->get_description = probe_desc;
	mc->get_version = probe_version;
}

static void
test_probe_init(TestProbe *self)
{
	(void)self;
}

static gboolean
intercept(
	GowlCompositor *compositor,
	guint           mods,
	guint           keysym,
	guint           raw_keycode,
	gboolean        pressed,
	gpointer        data
){
	Seen s;

	(void)compositor;
	(void)keysym;
	(void)data;
	s.code = raw_keycode;
	s.pressed = pressed;
	s.mods = mods;
	g_array_append_val(seen_keys, s);
	return FALSE;
}

static gboolean
custom_action(
	GowlCompositor *compositor,
	const gchar    *arg,
	gpointer        data
){
	(void)compositor;
	(void)data;
	g_ptr_array_add(seen_actions, g_strdup(arg));
	return TRUE;
}

/* ── Fake devices ───────────────────────────────────────────────── */

static struct wlr_keyboard_impl fake_keyboard_impl;
static struct wlr_pointer_impl  fake_pointer_impl;

/* ── The rig ────────────────────────────────────────────────────── */

typedef struct {
	gchar             *runtime;
	GowlConfig        *config;
	GowlModuleManager *modules;
	GowlCompositor    *compositor;
	GPtrArray         *keyboards;   /* struct wlr_keyboard*, owned */
	GPtrArray         *pointers;    /* struct wlr_pointer*, owned */
	gboolean           up;
} Rig;

static void
rig_setup(
	Rig           *r,
	gconstpointer  data
){
	gboolean with_module;
	const gchar *parent;
	GError *error = NULL;

	with_module = GPOINTER_TO_INT(data);
	memset(r, 0, sizeof *r);

	seen_keys = g_array_new(FALSE, TRUE, sizeof(Seen));
	seen_buttons = g_array_new(FALSE, TRUE, sizeof(Seen));
	seen_actions = g_ptr_array_new_with_free_func(g_free);
	r->keyboards = g_ptr_array_new();
	r->pointers = g_ptr_array_new();
	fake_keyboard_impl.name = "gowl-test-keyboard";
	fake_pointer_impl.name = "gowl-test-pointer";

	parent = g_getenv("XDG_RUNTIME_DIR");
	if (parent == NULL || !g_file_test(parent, G_FILE_TEST_IS_DIR))
		parent = g_get_tmp_dir();
	r->runtime = g_build_filename(parent, "gowl-remap-XXXXXX", NULL);
	g_assert_nonnull(g_mkdtemp_full(r->runtime, 0700));
	g_setenv("GOWL_DISABLE_SYSTEMD", "1", TRUE);
	g_setenv("XDG_RUNTIME_DIR", r->runtime, TRUE);
	g_setenv("WLR_BACKENDS", "headless", TRUE);
	g_setenv("WLR_HEADLESS_OUTPUTS", "1", TRUE);
	g_setenv("WLR_RENDERER", "pixman", TRUE);

	r->config = gowl_config_new();
	gowl_config_set_lock_command(r->config, "");
	r->modules = gowl_module_manager_new();

	/* The real module, and the probe beside it */
	if (with_module) {
		if (!gowl_module_manager_load_module(r->modules,
		                                     GOWL_TEST_INPUTREMAP_MODULE,
		                                     &error)) {
			g_test_skip("inputremap.so did not load");
			g_clear_error(&error);
			return;
		}
	}
	g_assert_true(gowl_module_manager_register(r->modules,
	                                           TEST_TYPE_PROBE, &error));
	g_assert_no_error(error);
	gowl_module_manager_activate_all(r->modules);

	r->compositor = gowl_compositor_new();
	gowl_compositor_set_config(r->compositor, r->config);
	gowl_compositor_set_module_manager(r->compositor, r->modules);
	if (!gowl_compositor_start(r->compositor, &error)) {
		g_test_skip("no headless compositor here");
		g_clear_error(&error);
		return;
	}
	gowl_module_manager_dispatch_startup(r->modules, r->compositor);
	gowl_compositor_set_key_intercept(r->compositor, intercept, NULL);
	gowl_compositor_set_custom_action_handler(r->compositor, custom_action,
	                                          NULL);
	r->up = TRUE;
}

static void
rig_teardown(
	Rig           *r,
	gconstpointer  data
){
	guint i;

	(void)data;
	/* Devices first, while the compositor they were handed to exists */
	for (i = 0; i < r->keyboards->len; i++) {
		struct wlr_keyboard *kb = g_ptr_array_index(r->keyboards, i);

		wlr_keyboard_finish(kb);
		g_free(kb);
	}
	for (i = 0; i < r->pointers->len; i++) {
		struct wlr_pointer *p = g_ptr_array_index(r->pointers, i);

		wlr_pointer_finish(p);
		g_free(p);
	}
	g_ptr_array_unref(r->keyboards);
	g_ptr_array_unref(r->pointers);
	g_clear_object(&r->compositor);
	g_clear_object(&r->modules);
	g_clear_object(&r->config);
	if (r->runtime != NULL)
		g_rmdir(r->runtime);
	g_free(r->runtime);
	g_array_unref(seen_keys);
	g_array_unref(seen_buttons);
	g_ptr_array_unref(seen_actions);
}

#define RIG_UP(r) do { if (!(r)->up) return; } while (0)

/* Plugs a keyboard in, as the backend's new_input would. */
static struct wlr_keyboard *
plug_keyboard(
	Rig         *r,
	const gchar *name
){
	struct wlr_keyboard *kb;

	kb = g_new0(struct wlr_keyboard, 1);
	wlr_keyboard_init(kb, &fake_keyboard_impl, name);
	g_ptr_array_add(r->keyboards, kb);
	wl_signal_emit_mutable(&r->compositor->backend->events.new_input,
	                       &kb->base);
	return kb;
}

static struct wlr_pointer *
plug_pointer(
	Rig         *r,
	const gchar *name
){
	struct wlr_pointer *p;

	p = g_new0(struct wlr_pointer, 1);
	wlr_pointer_init(p, &fake_pointer_impl, name);
	g_ptr_array_add(r->pointers, p);
	wl_signal_emit_mutable(&r->compositor->backend->events.new_input,
	                       &p->base);
	return p;
}

/* A key edge on the device itself -- the hardware path. */
static void
key(
	struct wlr_keyboard *kb,
	guint32              code,
	gboolean             pressed
){
	struct wlr_keyboard_key_event ev;

	memset(&ev, 0, sizeof ev);
	ev.time_msec = 1;
	ev.keycode = code;
	ev.update_state = TRUE;
	ev.state = pressed ? WL_KEYBOARD_KEY_STATE_PRESSED
	                   : WL_KEYBOARD_KEY_STATE_RELEASED;
	wlr_keyboard_notify_key(kb, &ev);
}

static void
tap(
	struct wlr_keyboard *kb,
	guint32              code
){
	key(kb, code, TRUE);
	key(kb, code, FALSE);
}

static void
button(
	struct wlr_pointer *p,
	guint32             code,
	gboolean            pressed
){
	struct wlr_pointer_button_event ev;

	memset(&ev, 0, sizeof ev);
	ev.pointer = p;
	ev.time_msec = 1;
	ev.button = code;
	ev.state = pressed ? WL_POINTER_BUTTON_STATE_PRESSED
	                   : WL_POINTER_BUTTON_STATE_RELEASED;
	wl_signal_emit_mutable(&p->events.button, &ev);
}

static void
wheel(
	struct wlr_pointer *p,
	gint32              v120
){
	struct wlr_pointer_axis_event ev;

	memset(&ev, 0, sizeof ev);
	ev.pointer = p;
	ev.time_msec = 1;
	ev.source = WL_POINTER_AXIS_SOURCE_WHEEL;
	ev.orientation = WL_POINTER_AXIS_VERTICAL_SCROLL;
	ev.delta = (gdouble)v120 / 8.0;
	ev.delta_discrete = v120;
	wl_signal_emit_mutable(&p->events.axis, &ev);
}

static void
ipc_ok(
	Rig         *r,
	const gchar *line
){
	g_autofree gchar *reply = NULL;

	reply = gowl_compositor_run_command(r->compositor, line);
	g_assert_nonnull(reply);
	if (!g_str_has_prefix(reply, "OK"))
		g_test_message("%s -> %s", line, reply);
	g_assert_true(g_str_has_prefix(reply, "OK"));
}

static void
clear_seen(void)
{
	g_array_set_size(seen_keys, 0);
	g_array_set_size(seen_buttons, 0);
	g_ptr_array_set_size(seen_actions, 0);
}

static void
assert_key(
	guint    i,
	guint32  code,
	gboolean pressed
){
	Seen *s;

	g_assert_cmpuint(seen_keys->len, >, i);
	s = &g_array_index(seen_keys, Seen, i);
	g_assert_cmpuint(s->code, ==, code);
	g_assert_cmpint(s->pressed, ==, pressed);
}

static void
assert_button(
	guint    i,
	guint32  code,
	gboolean pressed
){
	Seen *s;

	g_assert_cmpuint(seen_buttons->len, >, i);
	s = &g_array_index(seen_buttons, Seen, i);
	g_assert_cmpuint(s->code, ==, code);
	g_assert_cmpint(s->pressed, ==, pressed);
}

#define PEDAL_RULE \
	"inputremap-add {name: pedals, match: {name: \"Test Pedal\"}, " \
	"map: {KEY_A: {action: custom, arg: pedal-a}, " \
	"KEY_B: {key: KEY_F13}, KEY_C: {button: middle}, KEY_D: drop}}"

/* ── Tests ──────────────────────────────────────────────────────── */

/*
 * No module: the keyboard joins the group and its keys arrive exactly
 * as before.  The remap core never allocates anything.
 */
static void
test_no_module_unchanged(
	Rig           *r,
	gconstpointer  data
){
	struct wlr_keyboard *kb;

	(void)data;
	RIG_UP(r);
	kb = plug_keyboard(r, "Test Pedal");
	g_assert_true(kb->group == r->compositor->wlr_kb_group);

	tap(kb, KEY_A);
	g_assert_cmpuint(seen_keys->len, ==, 2);
	assert_key(0, KEY_A, TRUE);
	assert_key(1, KEY_A, FALSE);

	gowl_compositor_input_remap_reevaluate(r->compositor);
	g_assert_null(r->compositor->remap_devices);
	g_assert_true(kb->group == r->compositor->wlr_kb_group);
}

/* Module loaded, no rule: still unchanged. */
static void
test_module_without_rules(
	Rig           *r,
	gconstpointer  data
){
	struct wlr_keyboard *kb;

	(void)data;
	RIG_UP(r);
	kb = plug_keyboard(r, "Test Pedal");
	g_assert_true(kb->group == r->compositor->wlr_kb_group);
	tap(kb, KEY_A);
	g_assert_cmpuint(seen_keys->len, ==, 2);
	assert_key(0, KEY_A, TRUE);
	g_assert_cmpuint(seen_actions->len, ==, 0);
}

/* A rule added at runtime claims a device already plugged in, and
 * removing it gives the device back. */
static void
test_claim_release_on_rule_change(
	Rig           *r,
	gconstpointer  data
){
	struct wlr_keyboard *pedal;
	struct wlr_keyboard *board;

	(void)data;
	RIG_UP(r);
	pedal = plug_keyboard(r, "Test Pedal");
	board = plug_keyboard(r, "Test Keyboard");
	g_assert_true(pedal->group == r->compositor->wlr_kb_group);

	ipc_ok(r, PEDAL_RULE);
	g_assert_null(pedal->group);
	g_assert_true(board->group == r->compositor->wlr_kb_group);

	/* the other keyboard's KEY_A is untouched */
	tap(board, KEY_A);
	g_assert_cmpuint(seen_keys->len, ==, 2);
	assert_key(0, KEY_A, TRUE);
	g_assert_cmpuint(seen_actions->len, ==, 0);

	ipc_ok(r, "inputremap-remove pedals");
	g_assert_true(pedal->group == r->compositor->wlr_kb_group);

	/* and the pedal's KEY_A is a key again */
	clear_seen();
	tap(pedal, KEY_A);
	g_assert_cmpuint(seen_keys->len, ==, 2);
	assert_key(0, KEY_A, TRUE);
	g_assert_cmpuint(seen_actions->len, ==, 0);
}

/* A device plugged in after the rule is claimed on hotplug. */
static void
test_claim_on_hotplug(
	Rig           *r,
	gconstpointer  data
){
	struct wlr_keyboard *pedal;

	(void)data;
	RIG_UP(r);
	ipc_ok(r, PEDAL_RULE);
	pedal = plug_keyboard(r, "Test Pedal");
	g_assert_null(pedal->group);
	/* a pedal-only session still has a keyboard on the seat */
	g_assert_true(r->compositor->wlr_seat->capabilities
	              & WL_SEAT_CAPABILITY_KEYBOARD);
}

/* Every declarative output, one press each. */
static void
test_outputs(
	Rig           *r,
	gconstpointer  data
){
	struct wlr_keyboard *pedal;

	(void)data;
	RIG_UP(r);
	ipc_ok(r, PEDAL_RULE);
	pedal = plug_keyboard(r, "Test Pedal");

	/* action: runs once, on press; the release does nothing */
	tap(pedal, KEY_A);
	g_assert_cmpuint(seen_actions->len, ==, 1);
	g_assert_cmpstr(g_ptr_array_index(seen_actions, 0), ==, "pedal-a");
	g_assert_cmpuint(seen_keys->len, ==, 0);

	/* key: one press, one release, of the target key */
	clear_seen();
	tap(pedal, KEY_B);
	g_assert_cmpuint(seen_keys->len, ==, 2);
	assert_key(0, KEY_F13, TRUE);
	assert_key(1, KEY_F13, FALSE);
	g_assert_cmpuint(r->compositor->wlr_kb_group->keyboard.num_keycodes,
	                 ==, 0);

	/* button: a middle click through the button pipeline */
	clear_seen();
	tap(pedal, KEY_C);
	g_assert_cmpuint(seen_keys->len, ==, 0);
	g_assert_cmpuint(seen_buttons->len, ==, 2);
	assert_button(0, BTN_MIDDLE, TRUE);
	assert_button(1, BTN_MIDDLE, FALSE);

	/* drop: nothing at all */
	clear_seen();
	tap(pedal, KEY_D);
	g_assert_cmpuint(seen_keys->len, ==, 0);
	g_assert_cmpuint(seen_buttons->len, ==, 0);
	g_assert_cmpuint(seen_actions->len, ==, 0);

	/* pass: an unmentioned key goes through as itself */
	tap(pedal, KEY_Z);
	g_assert_cmpuint(seen_keys->len, ==, 2);
	assert_key(0, KEY_Z, TRUE);
	assert_key(1, KEY_Z, FALSE);
}

/* unmatched: drop swallows every key the rule does not name. */
static void
test_unmatched_drop(
	Rig           *r,
	gconstpointer  data
){
	struct wlr_keyboard *pedal;

	(void)data;
	RIG_UP(r);
	ipc_ok(r, "inputremap-add {name: mute, match: {name: \"Test Pedal\"}, "
	          "unmatched: drop, map: {KEY_A: pass}}");
	pedal = plug_keyboard(r, "Test Pedal");
	tap(pedal, KEY_Z);
	g_assert_cmpuint(seen_keys->len, ==, 0);
	tap(pedal, KEY_A);
	g_assert_cmpuint(seen_keys->len, ==, 2);
}

/*
 * The release mirrors its press.  The rule changes while the pedal is
 * down: the release must still release F13, not act on the new rule.
 */
static void
test_release_mirrors_press(
	Rig           *r,
	gconstpointer  data
){
	struct wlr_keyboard *pedal;

	(void)data;
	RIG_UP(r);
	ipc_ok(r, PEDAL_RULE);
	pedal = plug_keyboard(r, "Test Pedal");

	key(pedal, KEY_B, TRUE);
	ipc_ok(r, "inputremap-add {name: pedals, match: {name: \"Test Pedal\"}, "
	          "map: {KEY_B: {action: custom, arg: changed}}}");
	key(pedal, KEY_B, FALSE);

	g_assert_cmpuint(seen_keys->len, ==, 2);
	assert_key(0, KEY_F13, TRUE);
	assert_key(1, KEY_F13, FALSE);
	g_assert_cmpuint(seen_actions->len, ==, 0);

	/* a rule removed mid-press releases what it held, once */
	clear_seen();
	key(pedal, KEY_B, TRUE);     /* now the new action */
	g_assert_cmpuint(seen_actions->len, ==, 1);
	ipc_ok(r, PEDAL_RULE);
	key(pedal, KEY_C, TRUE);     /* middle down */
	ipc_ok(r, "inputremap-remove pedals");
	g_assert_cmpuint(seen_buttons->len, ==, 2);
	assert_button(1, BTN_MIDDLE, FALSE);
}

/* Super+Escape on a claimed device reaches the pipeline as such, even
 * when the rule drops both keys. */
static void
test_escape_hatch(
	Rig           *r,
	gconstpointer  data
){
	struct wlr_keyboard *pedal;
	Seen *s;

	(void)data;
	RIG_UP(r);
	ipc_ok(r, "inputremap-add {name: all, match: {name: \"Test Pedal\"}, "
	          "unmatched: drop, map: {KEY_LEFTMETA: drop, KEY_ESC: drop}}");
	pedal = plug_keyboard(r, "Test Pedal");

	key(pedal, KEY_LEFTMETA, TRUE);
	g_assert_cmpuint(seen_keys->len, ==, 0);
	key(pedal, KEY_ESC, TRUE);
	g_assert_cmpuint(seen_keys->len, ==, 1);
	s = &g_array_index(seen_keys, Seen, 0);
	g_assert_cmpuint(s->code, ==, KEY_ESC);
	g_assert_true(s->pressed);
	g_assert_true((s->mods & WLR_MODIFIER_LOGO) != 0);
	key(pedal, KEY_ESC, FALSE);
	key(pedal, KEY_LEFTMETA, FALSE);
	assert_key(1, KEY_ESC, FALSE);
	/* and Super is not left held in the shared state */
	g_assert_cmpuint(wlr_keyboard_get_modifiers(
		&r->compositor->wlr_kb_group->keyboard) & WLR_MODIFIER_LOGO, ==, 0);
}

/* Unplugged while holding a remapped key: the key is released. */
static void
test_destroy_while_held(
	Rig           *r,
	gconstpointer  data
){
	struct wlr_keyboard *pedal;

	(void)data;
	RIG_UP(r);
	ipc_ok(r, PEDAL_RULE);
	pedal = plug_keyboard(r, "Test Pedal");
	key(pedal, KEY_B, TRUE);
	g_assert_cmpuint(seen_keys->len, ==, 1);

	g_ptr_array_remove(r->keyboards, pedal);
	wlr_keyboard_finish(pedal);
	g_free(pedal);

	g_assert_cmpuint(seen_keys->len, ==, 2);
	assert_key(1, KEY_F13, FALSE);
	g_assert_cmpuint(g_hash_table_size(r->compositor->remap_devices), ==, 0);
}

/* A claimed mouse: its buttons remapped, another mouse untouched. */
static void
test_pointer_buttons(
	Rig           *r,
	gconstpointer  data
){
	struct wlr_pointer *pedal;
	struct wlr_pointer *mouse;

	(void)data;
	RIG_UP(r);
	ipc_ok(r, "inputremap-add {name: pm, match: {name: \"Test Pedal Mouse\", "
	          "type: pointer}, map: {BTN_LEFT: {button: middle}, "
	          "BTN_RIGHT: {action: custom, arg: pedal-b}, "
	          "WHEEL_UP: {action: custom, arg: notch}}}");
	pedal = plug_pointer(r, "Test Pedal Mouse");
	mouse = plug_pointer(r, "Test Mouse");

	button(pedal, BTN_LEFT, TRUE);
	button(pedal, BTN_LEFT, FALSE);
	g_assert_cmpuint(seen_buttons->len, ==, 2);
	assert_button(0, BTN_MIDDLE, TRUE);
	assert_button(1, BTN_MIDDLE, FALSE);

	button(pedal, BTN_RIGHT, TRUE);
	button(pedal, BTN_RIGHT, FALSE);
	g_assert_cmpuint(seen_actions->len, ==, 1);
	g_assert_cmpuint(seen_buttons->len, ==, 2);

	/* an unmentioned button on the claimed mouse passes as itself */
	button(pedal, BTN_SIDE, TRUE);
	button(pedal, BTN_SIDE, FALSE);
	assert_button(2, BTN_SIDE, TRUE);
	assert_button(3, BTN_SIDE, FALSE);

	/* the other mouse's left button is a left button */
	clear_seen();
	button(mouse, BTN_LEFT, TRUE);
	button(mouse, BTN_LEFT, FALSE);
	assert_button(0, BTN_LEFT, TRUE);

	/* whole notches: four quarter-notches are one input */
	clear_seen();
	wheel(pedal, -30);
	wheel(pedal, -30);
	wheel(pedal, -30);
	g_assert_cmpuint(seen_actions->len, ==, 0);
	wheel(pedal, -30);
	g_assert_cmpuint(seen_actions->len, ==, 1);
	wheel(pedal, -240);
	g_assert_cmpuint(seen_actions->len, ==, 3);
}

/* The listing shows every device and which are claimed. */
static void
test_list_devices(
	Rig           *r,
	gconstpointer  data
){
	g_autoptr(GPtrArray) devices = NULL;
	g_autofree gchar *reply = NULL;
	guint claimed;
	guint i;

	(void)data;
	RIG_UP(r);
	plug_keyboard(r, "Test Pedal");
	plug_keyboard(r, "Test Keyboard");
	ipc_ok(r, PEDAL_RULE);

	devices = gowl_compositor_list_input_devices(r->compositor);
	g_assert_cmpuint(devices->len, ==, 2);
	claimed = 0;
	for (i = 0; i < devices->len; i++) {
		GowlInputDeviceInfo *info = g_ptr_array_index(devices, i);

		if (info->claimed) {
			claimed++;
			g_assert_cmpstr(info->name, ==, "Test Pedal");
		}
	}
	g_assert_cmpuint(claimed, ==, 1);

	reply = gowl_compositor_run_command(r->compositor, "inputremap-devices");
	g_assert_nonnull(strstr(reply, "\"Test Pedal\""));
	g_assert_nonnull(strstr(reply, "\"claimed\":true"));
}

/* The module refuses a macro over IPC exactly as the parser does, and
 * disabling it releases everything. */
static void
test_ipc_refusals_and_disable(
	Rig           *r,
	gconstpointer  data
){
	struct wlr_keyboard *pedal;
	g_autofree gchar *reply = NULL;

	(void)data;
	RIG_UP(r);
	reply = gowl_compositor_run_command(r->compositor,
		"inputremap-add {name: m, match: {name: x}, "
		"map: {KEY_A: [{key: a}, {key: b}]}}");
	g_assert_true(g_str_has_prefix(reply, "ERROR"));
	g_assert_nonnull(strstr(reply, "one output"));

	ipc_ok(r, PEDAL_RULE);
	pedal = plug_keyboard(r, "Test Pedal");
	g_assert_null(pedal->group);
	ipc_ok(r, "inputremap-disable");
	g_assert_true(pedal->group == r->compositor->wlr_kb_group);
	ipc_ok(r, "inputremap-enable");
	g_assert_null(pedal->group);
}

int
main(
	int    argc,
	char **argv
){
	g_test_init(&argc, &argv, NULL);

#define ADD(path, with, fn) \
	g_test_add("/input-remap/claim/" path, Rig, GINT_TO_POINTER(with), \
	           rig_setup, fn, rig_teardown)

	ADD("no-module-unchanged", FALSE, test_no_module_unchanged);
	ADD("module-without-rules", TRUE, test_module_without_rules);
	ADD("claim-release-on-rule-change", TRUE,
	    test_claim_release_on_rule_change);
	ADD("claim-on-hotplug", TRUE, test_claim_on_hotplug);
	ADD("outputs", TRUE, test_outputs);
	ADD("unmatched-drop", TRUE, test_unmatched_drop);
	ADD("release-mirrors-press", TRUE, test_release_mirrors_press);
	ADD("escape-hatch", TRUE, test_escape_hatch);
	ADD("destroy-while-held", TRUE, test_destroy_while_held);
	ADD("pointer-buttons", TRUE, test_pointer_buttons);
	ADD("list-devices", TRUE, test_list_devices);
	ADD("ipc-refusals-and-disable", TRUE, test_ipc_refusals_and_disable);

#undef ADD
	return g_test_run();
}
