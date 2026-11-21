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
 * test-input-remap-gamepad.c - Game controllers, end to end.
 *
 * A headless compositor with the real inputremap module, and a REAL
 * evdev gamepad as the source: a uinput device this test creates (named
 * "gowl test source pad", id 1209:0008 from pid.codes' test range, so
 * no rule anyone has would open it).  Events are written into it and
 * read back out of the compositor's pipeline -- keys at the embedder
 * intercept, buttons and scroll at a probe mouse handler, motion as the
 * cursor moving.  The other direction too: a software keypad drives a
 * virtual pad, whose evdev node is opened and read back with
 * EVIOCGABS/EVIOCGKEY.  Last, /proc/bus/input/devices must hold no
 * `gowl virtual pad:' after the module is disabled and the compositor
 * is gone.
 *
 * Skips (cleanly) where /dev/uinput is not writable, or where the
 * evdev node of the device it made cannot be read (no uaccess ACL).
 */

#include <glib.h>
#include <glib/gstdio.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <wayland-server-core.h>
#include <wlr/interfaces/wlr_keyboard.h>

#include "gowl.h"
#include "core/gowl-core-private.h"

#define SRC_NAME    "gowl test source pad"
#define SRC_VENDOR  (0x1209)
#define SRC_PRODUCT (0x0008)
#define OUT_PAD     "gowl virtual pad: test out"

/* ── A probe module: buttons and scroll at the end of the pipeline ── */

#define TEST_TYPE_PAD_PROBE (test_pad_probe_get_type())
G_DECLARE_FINAL_TYPE(TestPadProbe, test_pad_probe, TEST, PAD_PROBE, GowlModule)

struct _TestPadProbe {
	GowlModule parent_instance;
};

typedef struct {
	guint32  code;
	gboolean pressed;
} Seen;

static GArray *seen_keys;
static GArray *seen_buttons;
static gint    seen_scroll;     /* value120 sum, vertical */

static gboolean
probe_button(
	GowlMouseHandler *h,
	guint             button,
	guint             state,
	guint             mods
){
	Seen s;

	(void)h;
	(void)mods;
	s.code = button;
	s.pressed = state != 0;
	g_array_append_val(seen_buttons, s);
	return FALSE;
}

static gboolean
probe_axis(
	GowlMouseHandler *h,
	guint             axis,
	gdouble           delta,
	gint              discrete,
	guint             mods
){
	(void)h;
	(void)delta;
	(void)mods;
	if (axis == 0)
		seen_scroll += discrete;
	return TRUE;
}

static void
probe_mouse_init(GowlMouseHandlerInterface *iface)
{
	iface->handle_button = probe_button;
	iface->handle_axis = probe_axis;
}

G_DEFINE_TYPE_WITH_CODE(TestPadProbe, test_pad_probe, GOWL_TYPE_MODULE,
	G_IMPLEMENT_INTERFACE(GOWL_TYPE_MOUSE_HANDLER, probe_mouse_init))

static gboolean probe_activate(GowlModule *m) { (void)m; return TRUE; }
static const gchar *probe_name(GowlModule *m) { (void)m; return "test-pad-probe"; }
static const gchar *probe_desc(GowlModule *m) { (void)m; return "probe"; }
static const gchar *probe_version(GowlModule *m) { (void)m; return "0"; }

static void
test_pad_probe_class_init(TestPadProbeClass *klass)
{
	GowlModuleClass *mc = GOWL_MODULE_CLASS(klass);

	mc->activate = probe_activate;
	mc->get_name = probe_name;
	mc->get_description = probe_desc;
	mc->get_version = probe_version;
}

static void
test_pad_probe_init(TestPadProbe *self)
{
	(void)self;
}

static gboolean
intercept(
	GowlCompositor *c,
	guint           mods,
	guint           keysym,
	guint           raw_keycode,
	gboolean        pressed,
	gpointer        data
){
	Seen s;

	(void)c;
	(void)mods;
	(void)keysym;
	(void)data;
	s.code = raw_keycode;
	s.pressed = pressed;
	g_array_append_val(seen_keys, s);
	return FALSE;
}

/* ── uinput: the source pad ──────────────────────────────────────── */

static int
source_pad_create(void)
{
	static const guint buttons[] = { BTN_SOUTH, BTN_EAST, BTN_NORTH, BTN_WEST };
	static const guint sticks[] = { ABS_X, ABS_Y, ABS_RX, ABS_RY };
	struct uinput_setup setup;
	int fd;
	guint i;

	fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0)
		return -1;
	ioctl(fd, UI_SET_EVBIT, EV_KEY);
	for (i = 0; i < G_N_ELEMENTS(buttons); i++)
		ioctl(fd, UI_SET_KEYBIT, buttons[i]);
	ioctl(fd, UI_SET_EVBIT, EV_ABS);
	for (i = 0; i < G_N_ELEMENTS(sticks); i++) {
		struct uinput_abs_setup abs;

		memset(&abs, 0, sizeof abs);
		abs.code = (__u16)sticks[i];
		abs.absinfo.minimum = -32768;
		abs.absinfo.maximum = 32767;
		abs.absinfo.flat = 128;
		ioctl(fd, UI_SET_ABSBIT, sticks[i]);
		ioctl(fd, UI_ABS_SETUP, &abs);
	}
	memset(&setup, 0, sizeof setup);
	g_strlcpy(setup.name, SRC_NAME, sizeof setup.name);
	setup.id.bustype = BUS_VIRTUAL;
	setup.id.vendor = SRC_VENDOR;
	setup.id.product = SRC_PRODUCT;
	if (ioctl(fd, UI_DEV_SETUP, &setup) < 0 || ioctl(fd, UI_DEV_CREATE) < 0) {
		close(fd);
		return -1;
	}
	return fd;
}

static void
source_emit(
	int     fd,
	guint16 type,
	guint16 code,
	gint32  value
){
	struct input_event ev;

	memset(&ev, 0, sizeof ev);
	ev.type = type;
	ev.code = code;
	ev.value = value;
	g_assert_cmpint(write(fd, &ev, sizeof ev), ==, (gssize)sizeof ev);
	memset(&ev, 0, sizeof ev);
	ev.type = EV_SYN;
	ev.code = SYN_REPORT;
	g_assert_cmpint(write(fd, &ev, sizeof ev), ==, (gssize)sizeof ev);
}

/* The /dev/input/eventN of the device called @name, or NULL. */
static gchar *
find_node(const gchar *name)
{
	GDir *dir;
	const gchar *entry;
	gchar *out;

	out = NULL;
	dir = g_dir_open("/sys/class/input", 0, NULL);
	if (dir == NULL)
		return NULL;
	while (out == NULL && (entry = g_dir_read_name(dir)) != NULL) {
		g_autofree gchar *path = NULL;
		g_autofree gchar *text = NULL;

		if (!g_str_has_prefix(entry, "event"))
			continue;
		path = g_build_filename("/sys/class/input", entry, "device", "name",
		                        NULL);
		if (g_file_get_contents(path, &text, NULL, NULL)
		    && g_strcmp0(g_strchomp(text), name) == 0)
			out = g_build_filename("/dev/input", entry, NULL);
	}
	g_dir_close(dir);
	return out;
}

/* Waits (up to 3 s) for @name's node to exist and be readable: udev
 * applies the uaccess ACL just after the node appears. */
static gchar *
wait_readable(const gchar *name)
{
	gint i;

	for (i = 0; i < 300; i++) {
		g_autofree gchar *node = find_node(name);

		if (node != NULL && access(node, R_OK) == 0)
			return g_steal_pointer(&node);
		g_usleep(10000);
	}
	return NULL;
}

/* gowl virtual pads listed in /proc/bus/input/devices. */
static guint
count_virtual_pads(void)
{
	g_autofree gchar *text = NULL;
	const gchar *p;
	guint n;

	if (!g_file_get_contents("/proc/bus/input/devices", &text, NULL, NULL))
		return 0;
	n = 0;
	for (p = text; (p = strstr(p, OUT_PAD)) != NULL; p++)
		n++;
	return n;
}

/* ── The rig ────────────────────────────────────────────────────── */

typedef struct {
	gchar             *runtime;
	GowlConfig        *config;
	GowlModuleManager *modules;
	GowlCompositor    *compositor;
	int                src_fd;
	gboolean           up;
	GPtrArray         *keyboards;
	guint              pads_before;
} Rig;

static struct wlr_keyboard_impl fake_keyboard_impl;

static void
rig_setup(
	Rig           *r,
	gconstpointer  data
){
	const gchar *parent;
	GError *error = NULL;
	g_autofree gchar *src = NULL;

	(void)data;
	memset(r, 0, sizeof *r);
	r->src_fd = -1;
	r->keyboards = g_ptr_array_new();
	seen_keys = g_array_new(FALSE, TRUE, sizeof(Seen));
	seen_buttons = g_array_new(FALSE, TRUE, sizeof(Seen));
	seen_scroll = 0;
	fake_keyboard_impl.name = "gowl-test-keypad";

	if (access("/dev/uinput", W_OK) != 0) {
		g_test_skip("/dev/uinput is not writable here");
		return;
	}
	r->pads_before = count_virtual_pads();
	r->src_fd = source_pad_create();
	if (r->src_fd < 0) {
		g_test_skip("could not create a uinput source pad");
		return;
	}
	src = wait_readable(SRC_NAME);
	if (src == NULL) {
		g_test_skip("the source pad's node is not readable (no uaccess)");
		return;
	}

	parent = g_getenv("XDG_RUNTIME_DIR");
	if (parent == NULL || !g_file_test(parent, G_FILE_TEST_IS_DIR))
		parent = g_get_tmp_dir();
	r->runtime = g_build_filename(parent, "gowl-pad-XXXXXX", NULL);
	g_assert_nonnull(g_mkdtemp_full(r->runtime, 0700));
	g_setenv("GOWL_DISABLE_SYSTEMD", "1", TRUE);
	g_setenv("XDG_RUNTIME_DIR", r->runtime, TRUE);
	g_setenv("XDG_STATE_HOME", r->runtime, TRUE);
	g_setenv("WLR_BACKENDS", "headless", TRUE);
	g_setenv("WLR_HEADLESS_OUTPUTS", "1", TRUE);
	g_setenv("WLR_RENDERER", "pixman", TRUE);

	r->config = gowl_config_new();
	gowl_config_set_lock_command(r->config, "");
	r->modules = gowl_module_manager_new();
	if (!gowl_module_manager_load_module(r->modules,
	                                     GOWL_TEST_INPUTREMAP_MODULE, &error)) {
		g_test_skip("the inputremap module did not load");
		g_clear_error(&error);
		return;
	}
	g_assert_true(gowl_module_manager_register(r->modules,
	                                           TEST_TYPE_PAD_PROBE, &error));
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
	r->up = TRUE;
}

static void
rig_teardown(
	Rig           *r,
	gconstpointer  data
){
	guint i;

	(void)data;
	for (i = 0; i < r->keyboards->len; i++) {
		struct wlr_keyboard *kb = g_ptr_array_index(r->keyboards, i);

		wlr_keyboard_finish(kb);
		g_free(kb);
	}
	g_ptr_array_unref(r->keyboards);
	g_clear_object(&r->compositor);
	g_clear_object(&r->modules);
	g_clear_object(&r->config);
	if (r->src_fd >= 0) {
		ioctl(r->src_fd, UI_DEV_DESTROY);
		close(r->src_fd);
	}
	/* Every virtual pad the module made is gone with it */
	if (r->up)
		g_assert_cmpuint(count_virtual_pads(), ==, r->pads_before);
	if (r->runtime != NULL)
		g_rmdir(r->runtime);
	g_free(r->runtime);
	g_array_unref(seen_keys);
	g_array_unref(seen_buttons);
}

#define RIG_UP(r) do { if (!(r)->up) return; } while (0)

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

/* Runs the compositor's loop for @ms (the pad fd and the tick are on it) */
static void
pump(
	Rig  *r,
	gint  ms
){
	struct wl_event_loop *loop;
	gint64 end;

	loop = gowl_compositor_get_event_loop(r->compositor);
	end = g_get_monotonic_time() + (gint64)ms * 1000;
	while (g_get_monotonic_time() < end)
		wl_event_loop_dispatch(loop, 5);
}

/* Pumps until @cond, at most 2 s. */
#define PUMP_UNTIL(r, cond) do { \
	gint _i; \
	for (_i = 0; _i < 400 && !(cond); _i++) \
		pump((r), 5); \
} while (0)

#define PAD_RULE \
	"inputremap-add {name: pad, match: {id: \"1209:0008\", type: gamepad}," \
	" analog: {press: 0.5, release: 0.35, pointer-speed: 2000," \
	"          pointer-curve: 1.0, scroll-rate: 40}," \
	" map: {ABS_X+: {key: KEY_D}, ABS_X-: {key: KEY_A}," \
	"       pad-a: {button: left}, ABS_RX: {pointer: x}," \
	"       ABS_RY: {scroll: vertical}}}"

/* The pad is listed as a gamepad with its axes, and claimed by a rule */
static void
test_listed_and_claimed(
	Rig           *r,
	gconstpointer  data
){
	g_autofree gchar *before = NULL;
	g_autofree gchar *after = NULL;

	(void)data;
	RIG_UP(r);
	ipc_ok(r, PAD_RULE);
	after = gowl_compositor_run_command(r->compositor, "inputremap-devices");
	g_assert_nonnull(strstr(after, SRC_NAME));
	g_assert_nonnull(strstr(after, "\"type\":\"gamepad\""));
	g_assert_nonnull(strstr(after, "\"ABS_RX\""));
	g_assert_nonnull(strstr(after, "\"open\":true"));
	before = gowl_compositor_run_command(r->compositor, "inputremap-list");
	g_assert_nonnull(strstr(before, "\"continuous\":[\"ABS_RX: {pointer: x}\""));
}

/* A stick direction is one press, through hysteresis */
static void
test_stick_to_key(
	Rig           *r,
	gconstpointer  data
){
	(void)data;
	RIG_UP(r);
	ipc_ok(r, PAD_RULE);

	source_emit(r->src_fd, EV_ABS, ABS_X, 30000);
	PUMP_UNTIL(r, seen_keys->len >= 1);
	g_assert_cmpuint(seen_keys->len, ==, 1);
	g_assert_cmpuint(g_array_index(seen_keys, Seen, 0).code, ==, KEY_D);
	g_assert_true(g_array_index(seen_keys, Seen, 0).pressed);

	/* between the thresholds: still held, nothing new */
	source_emit(r->src_fd, EV_ABS, ABS_X, (gint32)(0.42 * 32767));
	pump(r, 30);
	g_assert_cmpuint(seen_keys->len, ==, 1);
	/* back to rest: released once */
	source_emit(r->src_fd, EV_ABS, ABS_X, 0);
	PUMP_UNTIL(r, seen_keys->len >= 2);
	g_assert_cmpuint(seen_keys->len, ==, 2);
	g_assert_cmpuint(g_array_index(seen_keys, Seen, 1).code, ==, KEY_D);
	g_assert_false(g_array_index(seen_keys, Seen, 1).pressed);
	/* the other way is the other key */
	source_emit(r->src_fd, EV_ABS, ABS_X, -32768);
	PUMP_UNTIL(r, seen_keys->len >= 3);
	g_assert_cmpuint(g_array_index(seen_keys, Seen, 2).code, ==, KEY_A);
	source_emit(r->src_fd, EV_ABS, ABS_X, 0);
	PUMP_UNTIL(r, seen_keys->len >= 4);
	g_assert_false(g_array_index(seen_keys, Seen, 3).pressed);
}

/* A pad button is a pointer button, release mirroring press */
static void
test_button(
	Rig           *r,
	gconstpointer  data
){
	(void)data;
	RIG_UP(r);
	ipc_ok(r, PAD_RULE);
	source_emit(r->src_fd, EV_KEY, BTN_SOUTH, 1);
	PUMP_UNTIL(r, seen_buttons->len >= 1);
	source_emit(r->src_fd, EV_KEY, BTN_SOUTH, 0);
	PUMP_UNTIL(r, seen_buttons->len >= 2);
	g_assert_cmpuint(seen_buttons->len, ==, 2);
	g_assert_cmpuint(g_array_index(seen_buttons, Seen, 0).code, ==, BTN_LEFT);
	g_assert_true(g_array_index(seen_buttons, Seen, 0).pressed);
	g_assert_false(g_array_index(seen_buttons, Seen, 1).pressed);
}

/* The right stick moves the pointer while deflected, and stops */
static void
test_stick_to_pointer_and_scroll(
	Rig           *r,
	gconstpointer  data
){
	gdouble x0;
	gdouble x1;
	gdouble x2;

	(void)data;
	RIG_UP(r);
	ipc_ok(r, PAD_RULE);
	gowl_compositor_warp_cursor(r->compositor, 100, 100);
	x0 = r->compositor->wlr_cursor->x;
	source_emit(r->src_fd, EV_ABS, ABS_RX, 32767);
	pump(r, 150);
	x1 = r->compositor->wlr_cursor->x;
	/* ~2000 px/s for 0.15 s; generous bounds for a loaded machine */
	g_assert_cmpfloat(x1 - x0, >, 50.0);
	source_emit(r->src_fd, EV_ABS, ABS_RX, 0);
	pump(r, 40);
	x1 = r->compositor->wlr_cursor->x;
	pump(r, 80);
	x2 = r->compositor->wlr_cursor->x;
	/* centred: it stops (the tick is not re-armed at rest) */
	g_assert_cmpfloat(x2, ==, x1);

	/* scroll: down is positive value120, whole notches */
	source_emit(r->src_fd, EV_ABS, ABS_RY, 32767);
	pump(r, 150);
	source_emit(r->src_fd, EV_ABS, ABS_RY, 0);
	pump(r, 30);
	g_assert_cmpint(seen_scroll, >, 0);
	g_assert_cmpint(seen_scroll % 120, ==, 0);
}

/* No rule: the controller is never opened */
static void
test_unmatched_never_opened(
	Rig           *r,
	gconstpointer  data
){
	g_autofree gchar *devs = NULL;

	(void)data;
	RIG_UP(r);
	ipc_ok(r, "inputremap-add {name: other, match: {id: \"dead:beef\"}, "
	          "map: {KEY_A: drop}}");
	devs = gowl_compositor_run_command(r->compositor, "inputremap-devices");
	g_assert_nonnull(strstr(devs, SRC_NAME));
	g_assert_null(strstr(devs, "\"open\":true"));
	source_emit(r->src_fd, EV_ABS, ABS_X, 30000);
	pump(r, 40);
	g_assert_cmpuint(seen_keys->len, ==, 0);
}

/* Reads one axis value of the output pad's node. */
static gint32
out_abs(
	int  fd,
	guint code
){
	struct input_absinfo ai;

	g_assert_cmpint(ioctl(fd, EVIOCGABS(code), &ai), ==, 0);
	return ai.value;
}

static gboolean
out_key(
	int   fd,
	guint code
){
	unsigned long bits[KEY_CNT / (8 * sizeof(unsigned long)) + 1];

	memset(bits, 0, sizeof bits);
	g_assert_cmpint(ioctl(fd, EVIOCGKEY(sizeof bits), bits), >=, 0);
	return (bits[code / (8 * sizeof(unsigned long))]
	        >> (code % (8 * sizeof(unsigned long)))) & 1UL;
}

static void
keypad_key(
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

/* A keypad as a controller: W+D up-right on the unit circle, A+D
 * cancelling, a pad button; then disabled, the pad is gone. */
static void
test_keys_to_virtual_pad(
	Rig           *r,
	gconstpointer  data
){
	struct wlr_keyboard *kp;
	g_autofree gchar *node = NULL;
	g_autofree gchar *status = NULL;
	int fd;

	(void)data;
	RIG_UP(r);
	ipc_ok(r, "inputremap-add {name: kp, match: {name: \"Test Keypad\"},"
	          " virtual-pad: {name: \"test out\"},"
	          " map: {KEY_W: {stick: left, y: -1}, KEY_S: {stick: left, y: 1},"
	          "       KEY_A: {stick: left, x: -1}, KEY_D: {stick: left, x: 1},"
	          "       KEY_SPACE: {pad-button: pad-a},"
	          "       KEY_E: {trigger: right, value: 0.5}}}");
	kp = g_new0(struct wlr_keyboard, 1);
	wlr_keyboard_init(kp, &fake_keyboard_impl, "Test Keypad");
	g_ptr_array_add(r->keyboards, kp);
	wl_signal_emit_mutable(&r->compositor->backend->events.new_input, &kp->base);

	/* lazily created: nothing yet */
	g_assert_cmpuint(count_virtual_pads(), ==, r->pads_before);
	keypad_key(kp, KEY_W, TRUE);
	node = wait_readable(OUT_PAD);
	if (node == NULL) {
		keypad_key(kp, KEY_W, FALSE);
		g_test_skip("the virtual pad's node is not readable (no uaccess)");
		return;
	}
	g_assert_cmpuint(count_virtual_pads(), ==, r->pads_before + 1);
	fd = open(node, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	g_assert_cmpint(fd, >=, 0);
	g_assert_cmpint(out_abs(fd, ABS_Y), ==, -32768);
	g_assert_cmpint(ABS(out_abs(fd, ABS_X)), <=, 1);

	/* W+D: up-right, on the unit circle */
	keypad_key(kp, KEY_D, TRUE);
	g_assert_cmpint(ABS(out_abs(fd, ABS_X) - 23170), <=, 2);
	g_assert_cmpint(ABS(out_abs(fd, ABS_Y) + 23170), <=, 2);
	/* +A: x cancels */
	keypad_key(kp, KEY_A, TRUE);
	g_assert_cmpint(ABS(out_abs(fd, ABS_X)), <=, 1);
	g_assert_cmpint(out_abs(fd, ABS_Y), ==, -32768);
	keypad_key(kp, KEY_A, FALSE);
	keypad_key(kp, KEY_D, FALSE);
	keypad_key(kp, KEY_W, FALSE);
	g_assert_cmpint(ABS(out_abs(fd, ABS_X)), <=, 1);
	g_assert_cmpint(ABS(out_abs(fd, ABS_Y)), <=, 1);

	/* a pad button, and a half-pulled trigger */
	keypad_key(kp, KEY_SPACE, TRUE);
	keypad_key(kp, KEY_E, TRUE);
	g_assert_true(out_key(fd, BTN_SOUTH));
	g_assert_cmpint(out_abs(fd, ABS_RZ), ==, 128);
	keypad_key(kp, KEY_SPACE, FALSE);
	keypad_key(kp, KEY_E, FALSE);
	g_assert_false(out_key(fd, BTN_SOUTH));
	g_assert_cmpint(out_abs(fd, ABS_RZ), ==, 0);
	close(fd);

	status = gowl_compositor_run_command(r->compositor, "inputremap-status");
	g_assert_nonnull(strstr(status, "pads=1"));

	/* disabled: the pad is unplugged */
	ipc_ok(r, "inputremap-disable");
	g_assert_cmpuint(count_virtual_pads(), ==, r->pads_before);
	/* and the keypad's keys go back to being keys */
	g_array_set_size(seen_keys, 0);
	keypad_key(kp, KEY_W, TRUE);
	keypad_key(kp, KEY_W, FALSE);
	g_assert_cmpuint(seen_keys->len, ==, 2);
}

/* Removing the rule that declares a pad destroys it, even mid-press:
 * the release still reaches it first. */
static void
test_rule_removed_destroys_pad(
	Rig           *r,
	gconstpointer  data
){
	struct wlr_keyboard *kp;

	(void)data;
	RIG_UP(r);
	ipc_ok(r, "inputremap-add {name: kp, match: {name: \"Test Keypad\"},"
	          " virtual-pad: {name: \"test out\"},"
	          " map: {KEY_SPACE: {pad-button: BTN_SOUTH}}}");
	kp = g_new0(struct wlr_keyboard, 1);
	wlr_keyboard_init(kp, &fake_keyboard_impl, "Test Keypad");
	g_ptr_array_add(r->keyboards, kp);
	wl_signal_emit_mutable(&r->compositor->backend->events.new_input, &kp->base);
	keypad_key(kp, KEY_SPACE, TRUE);
	if (count_virtual_pads() != r->pads_before + 1) {
		g_test_skip("no virtual pad was created");
		keypad_key(kp, KEY_SPACE, FALSE);
		return;
	}
	ipc_ok(r, "inputremap-remove kp");
	g_assert_cmpuint(count_virtual_pads(), ==, r->pads_before);
	keypad_key(kp, KEY_SPACE, FALSE);
}

int
main(
	int    argc,
	char **argv
){
	g_test_init(&argc, &argv, NULL);

#define ADD(path, fn) \
	g_test_add("/input-remap/gamepad/" path, Rig, NULL, \
	           rig_setup, fn, rig_teardown)

	ADD("listed-and-claimed", test_listed_and_claimed);
	ADD("stick-to-key", test_stick_to_key);
	ADD("button", test_button);
	ADD("stick-to-pointer-and-scroll", test_stick_to_pointer_and_scroll);
	ADD("unmatched-never-opened", test_unmatched_never_opened);
	ADD("keys-to-virtual-pad", test_keys_to_virtual_pad);
	ADD("rule-removed-destroys-pad", test_rule_removed_destroys_pad);

#undef ADD
	return g_test_run();
}
