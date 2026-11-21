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
 * gowl-input-remap-pad.c - Game controllers as remap sources.
 *
 * libinput ignores joysticks (udev tags them ID_INPUT_JOYSTICK and it
 * leaves them alone), so a gamepad never becomes a wlroots device: games
 * read /dev/input/eventN themselves.  To remap one, gowl has to find and
 * read it too.  This file does that, and nothing more -- every decision
 * still belongs to the active GowlInputRemapper, and every output goes
 * through gowl-input-remap-core.c and the compositor's own pipelines.
 *
 * Lifecycle, and the invariants it keeps:
 *
 *   - NOTHING happens without an active remapper.  The first
 *     gowl_input_remap_pads_reevaluate() with one creates the state,
 *     scans sysfs and starts an inotify watch on /dev/input; with none
 *     (and no state yet) it returns before touching anything.  The
 *     fast-path guard (tests/test-input-remap-fastpath.sh) holds that.
 *   - A controller is identified from SYSFS (name, ids, capability
 *     bitmaps) without opening its node, and OPENED only when the
 *     remapper claims it -- or, read-only and never grabbed, while the
 *     user runs identify mode.  No rule, never opened.
 *   - It is opened through the compositor's wlroots session (libseat:
 *     logind or seatd hand over the fd) when there is one, else with a
 *     plain open(); never as root without a session.
 *   - `grab: true' on the deciding rule takes it with EVIOCGRAB so the
 *     game stops seeing the physical pad; the default leaves it shared.
 *   - Its fd sits on the compositor's wl_event_loop, as does the 8 ms
 *     tick that drives continuous targets -- and the tick runs only
 *     while some stick mapped to the pointer or scroll is deflected.
 *   - gowl's own virtual pads (GOWL_INPUT_REMAP_PAD_NAME_PREFIX) are
 *     never read back as inputs, and a node libinput already owns is
 *     never read twice.
 *
 * Digital inputs -- buttons, and stick directions crossing the rule's
 * press threshold (with hysteresis back below its release threshold)
 * -- become GOWL_INPUT_REMAP_EVENT_PAD edges through
 * gowl_input_remap_core_source_edge(), so they get everything a pedal
 * gets: one press one output, the release mirroring the press, the
 * lock-aware pipelines.  Continuous targets (`pointer:', `scroll:') are
 * the declared exception and are driven here, by the tick, through
 * gowl_compositor_remap_handle_motion() / _axis().
 */

#include "gowl-core-private.h"
#include "interfaces/gowl-input-remapper.h"
#include "boxed/gowl-input-remap-rule.h"
#include "boxed/gowl-input-remap-analog.h"

#ifdef GOWL_HAVE_EVDEV_PADS

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <linux/input.h>
#include <json-glib/json-glib.h>
#include <wlr/backend/session.h>
#include <wlr/backend/libinput.h>

#define PAD_SYSFS_DIR      "/sys/class/input"
#define PAD_DEV_DIR        "/dev/input"
#define PAD_TICK_MS        (8)
#define PAD_BITS_PER_LONG  (sizeof(unsigned long) * 8)
#define PAD_NBITS(n)       (((n) + PAD_BITS_PER_LONG - 1) / PAD_BITS_PER_LONG)

/* What the deciding rule wants done with one whole axis, resolved when
 * the pad is claimed and again on every re-evaluation (rules change
 * only through one).  Values, never a pointer into a rule. */
typedef struct {
	GowlInputRemapTargetKind kind;       /* POINTER, SCROLL, or PASS */
	guint                    component;
} PadContinuous;

/* One absolute axis of an open controller. */
typedef struct {
	gboolean         present;
	GowlRemapAbsInfo abs;
	gint32           raw;
	gboolean         centred;
	gdouble          value;              /* deadzone applied, inverted */
	gboolean         down[3];            /* by GOWL_INPUT_REMAP_ABS_POS/NEG */
	gint64           deflected_since;    /* monotonic us, 0 at rest */
	PadContinuous    cont;
} PadAxis;

/* One joystick-like evdev node. */
typedef struct {
	GowlCompositor      *compositor;
	gchar               *sysname;        /* "event23" */
	gchar               *devnode;        /* "/dev/input/event23" */
	GowlInputDeviceInfo *info;           /* identity; id fixed at sight */
	unsigned long        keybits[PAD_NBITS(KEY_CNT)];
	unsigned long        absbits[PAD_NBITS(ABS_CNT)];

	int                     fd;
	struct wlr_device      *wdev;        /* when opened through the session */
	struct wl_event_source *source;
	gboolean                grabbed;
	gboolean                dropped;     /* SYN_DROPPED: resync at SYN_REPORT */
	gboolean                open_failed; /* retried on the node's IN_ATTRIB */

	gboolean             claimed;
	GowlRemapDevice     *rd;             /* the core's record while claimed */
	GowlInputRemapAnalog analog;         /* the deciding rule's settings */
	gboolean             want_grab;

	PadAxis              axes[ABS_CNT];
	guint64              dirty;          /* axes changed since SYN_REPORT */
	gdouble              scroll_acc[2];  /* value120 units, per orientation */
} GowlRemapPad;

/* compositor->remap_pads */
typedef struct {
	GowlCompositor         *compositor;
	GHashTable             *pads;        /* sysname -> GowlRemapPad* */
	int                     inotify_fd;
	struct wl_event_source *inotify_source;
	struct wl_event_source *tick;
	gboolean                ticking;
	gint64                  last_tick_us;
	gboolean                identifying;
	gboolean                warned_root;
	struct wl_listener      session_active;
	gboolean                session_listening;
} GowlRemapPads;

static void pad_close(GowlRemapPad *pad);
static void pad_apply(GowlRemapPads *st, GowlRemapPad *pad, GowlInputRemapper *r);
static void tick_arm(GowlRemapPads *st);

/* --- small helpers --- */

static gboolean
bit_test(
	const unsigned long *bits,
	guint                bit
){
	return (bits[bit / PAD_BITS_PER_LONG] >> (bit % PAD_BITS_PER_LONG)) & 1UL;
}

static GowlInputRemapper *
active_remapper(GowlCompositor *self)
{
	if (self->module_mgr == NULL)
		return NULL;
	return (GowlInputRemapper *)gowl_module_manager_get_input_remapper(
		self->module_mgr);
}

static guint32
now_msec(void)
{
	return (guint32)(g_get_monotonic_time() / 1000);
}

/* Reads a small sysfs file, trailing newline stripped. */
static gchar *
read_sysfs(
	const gchar *sysname,
	const gchar *leaf
){
	g_autofree gchar *path = NULL;
	gchar *text;

	path = g_build_filename(PAD_SYSFS_DIR, sysname, "device", leaf, NULL);
	if (!g_file_get_contents(path, &text, NULL, NULL))
		return NULL;
	g_strchomp(text);
	return text;
}

/*
 * A capability bitmap as sysfs prints it: space-separated hex words,
 * the MOST significant first, each one kernel `long' wide.
 */
static void
read_bitmap(
	const gchar   *sysname,
	const gchar   *leaf,
	unsigned long *bits,
	gsize          n_longs
){
	g_autofree gchar *text = NULL;
	g_auto(GStrv) words = NULL;
	guint n;
	guint i;

	memset(bits, 0, n_longs * sizeof *bits);
	text = read_sysfs(sysname, leaf);
	if (text == NULL)
		return;
	words = g_strsplit(text, " ", -1);
	n = g_strv_length(words);
	for (i = 0; i < n && i < n_longs; i++)
		bits[i] = (unsigned long)g_ascii_strtoull(words[n - 1 - i], NULL, 16);
}

/*
 * Whether a node is a game controller, and which kind.  The same idea
 * as udev's input_id: gamepad buttons make a gamepad; joystick buttons,
 * or X/Y with a joystick-only axis, make a joystick -- unless it is
 * really a tablet, a touch device or a mouse, which libinput owns.
 */
static gboolean
classify(
	const unsigned long      *keybits,
	const unsigned long      *absbits,
	const unsigned long      *relbits,
	GowlInputRemapDeviceType *out_type
){
	static const guint joy_axes[] = {
		ABS_Z, ABS_RX, ABS_RY, ABS_RZ, ABS_THROTTLE, ABS_RUDDER,
		ABS_WHEEL, ABS_GAS, ABS_BRAKE, ABS_HAT0X, ABS_HAT0Y
	};
	gboolean joy_buttons;
	gboolean joy_axis;
	gboolean xy;
	guint i;

	/* Tablets, tablet pads, touchscreens and mice: libinput's */
	if (bit_test(keybits, BTN_TOOL_PEN) || bit_test(keybits, BTN_STYLUS)
	    || bit_test(keybits, BTN_TOUCH) || bit_test(keybits, BTN_0)
	    || bit_test(absbits, ABS_MT_POSITION_X)
	    || bit_test(relbits, REL_X) || bit_test(relbits, REL_Y))
		return FALSE;

	if (bit_test(keybits, BTN_GAMEPAD)) {
		*out_type = GOWL_INPUT_REMAP_DEVICE_GAMEPAD;
		return TRUE;
	}
	joy_buttons = FALSE;
	for (i = BTN_JOYSTICK; i < BTN_GAMEPAD; i++)
		if (bit_test(keybits, i))
			joy_buttons = TRUE;
	for (i = BTN_TRIGGER_HAPPY1; i <= BTN_TRIGGER_HAPPY40; i++)
		if (bit_test(keybits, i))
			joy_buttons = TRUE;
	joy_axis = FALSE;
	for (i = 0; i < G_N_ELEMENTS(joy_axes); i++)
		if (bit_test(absbits, joy_axes[i]))
			joy_axis = TRUE;
	xy = bit_test(absbits, ABS_X) && bit_test(absbits, ABS_Y);
	if (joy_buttons || (xy && joy_axis)) {
		*out_type = GOWL_INPUT_REMAP_DEVICE_JOYSTICK;
		return TRUE;
	}
	return FALSE;
}

/* Whether wlroots (libinput) already has a device on @sysname. */
typedef struct {
	const gchar *sysname;
	gboolean     found;
} OwnedQuery;

static void
check_owned(
	struct wlr_input_device *dev,
	gpointer                 user_data
){
	OwnedQuery *q = (OwnedQuery *)user_data;
	struct libinput_device *li;

	if (q->found || !wlr_input_device_is_libinput(dev))
		return;
	li = wlr_libinput_get_device_handle(dev);
	if (li != NULL && g_strcmp0(libinput_device_get_sysname(li),
	                            q->sysname) == 0)
		q->found = TRUE;
}

/* Sticks and hats rest in the middle; triggers, gas and brake rest at
 * their minimum unless their range is signed (or a rule says
 * otherwise -- a cheap pad's right stick on ABS_Z). */
static gboolean
axis_is_centred(
	guint                   code,
	const GowlRemapAbsInfo *abs,
	guint64                 forced
){
	if ((forced >> code) & 1)
		return TRUE;
	switch (code) {
	case ABS_Z:
	case ABS_RZ:
	case ABS_THROTTLE:
	case ABS_RUDDER:
	case ABS_WHEEL:
	case ABS_GAS:
	case ABS_BRAKE:
		return abs->min < 0;
	default:
		return TRUE;
	}
}

/* --- the state --- */

static void
pad_free(gpointer data)
{
	GowlRemapPad *pad = (GowlRemapPad *)data;

	if (pad == NULL)
		return;
	g_clear_pointer(&pad->rd, gowl_input_remap_core_source_free);
	pad_close(pad);
	g_clear_pointer(&pad->info, gowl_input_device_info_free);
	g_free(pad->sysname);
	g_free(pad->devnode);
	g_free(pad);
}

static void
on_session_active(
	struct wl_listener *listener,
	void               *data
){
	GowlRemapPads *st;

	(void)data;
	st = wl_container_of(listener, st, session_active);
	/* Back on our VT: logind revoked the fds when we left, so the pads
	 * that were open are reopened by re-deciding them. */
	if (st->compositor->session != NULL && st->compositor->session->active)
		gowl_input_remap_pads_reevaluate(st->compositor);
}

static int on_inotify(int fd, uint32_t mask, void *data);
static int on_tick(void *data);

static GowlRemapPads *
pads_state(GowlCompositor *self)
{
	GowlRemapPads *st;
	struct wl_event_loop *loop;

	if (self->remap_pads != NULL)
		return (GowlRemapPads *)self->remap_pads;

	st = g_new0(GowlRemapPads, 1);
	st->compositor = self;
	st->pads = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, pad_free);
	st->inotify_fd = -1;
	wl_list_init(&st->session_active.link);
	self->remap_pads = st;

	loop = gowl_compositor_get_event_loop(self);
	if (loop == NULL)
		return st;

	/* Hotplug: /dev/input itself, no udev.  IN_ATTRIB because the
	 * uaccess ACL lands just after the node is created, so a first
	 * open can fail with EACCES and succeed a moment later. */
	st->inotify_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	if (st->inotify_fd >= 0) {
		if (inotify_add_watch(st->inotify_fd, PAD_DEV_DIR,
		                      IN_CREATE | IN_DELETE | IN_ATTRIB) < 0) {
			g_message("input-remap: cannot watch %s for controllers: %s",
			          PAD_DEV_DIR, g_strerror(errno));
			close(st->inotify_fd);
			st->inotify_fd = -1;
		} else {
			st->inotify_source = wl_event_loop_add_fd(loop, st->inotify_fd,
				WL_EVENT_READABLE, on_inotify, st);
		}
	}
	st->tick = wl_event_loop_add_timer(loop, on_tick, st);

	if (self->session != NULL) {
		st->session_active.notify = on_session_active;
		wl_signal_add(&self->session->events.active, &st->session_active);
		st->session_listening = TRUE;
	}
	return st;
}

/* Everything off: every pad released and closed, the watch and the tick
 * removed, the state freed -- back to the fast path. */
static void
pads_destroy(GowlCompositor *self)
{
	GowlRemapPads *st;

	st = (GowlRemapPads *)self->remap_pads;
	if (st == NULL)
		return;
	g_hash_table_destroy(st->pads);
	if (st->inotify_source != NULL)
		wl_event_source_remove(st->inotify_source);
	if (st->inotify_fd >= 0)
		close(st->inotify_fd);
	if (st->tick != NULL)
		wl_event_source_remove(st->tick);
	if (st->session_listening)
		wl_list_remove(&st->session_active.link);
	g_free(st);
	self->remap_pads = NULL;
}

/* --- discovery --- */

/*
 * Looks at one /dev/input/eventN through sysfs.  A controller gets a
 * record (and an id) the first time it is seen; anything else is left
 * alone entirely.  Returns the record, or NULL.
 */
static GowlRemapPad *
pad_discover(
	GowlRemapPads *st,
	const gchar   *sysname
){
	GowlRemapPad *pad;
	GowlInputRemapDeviceType type;
	unsigned long keybits[PAD_NBITS(KEY_CNT)];
	unsigned long absbits[PAD_NBITS(ABS_CNT)];
	unsigned long relbits[PAD_NBITS(REL_CNT)];
	g_autofree gchar *name = NULL;
	g_autofree gchar *vendor = NULL;
	g_autofree gchar *product = NULL;
	OwnedQuery q;

	if (!g_str_has_prefix(sysname, "event"))
		return NULL;
	pad = g_hash_table_lookup(st->pads, sysname);
	if (pad != NULL)
		return pad;

	read_bitmap(sysname, "capabilities/key", keybits, G_N_ELEMENTS(keybits));
	read_bitmap(sysname, "capabilities/abs", absbits, G_N_ELEMENTS(absbits));
	read_bitmap(sysname, "capabilities/rel", relbits, G_N_ELEMENTS(relbits));
	if (!classify(keybits, absbits, relbits, &type))
		return NULL;

	name = read_sysfs(sysname, "name");
	/* gowl's own virtual pads are outputs, never inputs */
	if (name != NULL && g_str_has_prefix(name, GOWL_INPUT_REMAP_PAD_NAME_PREFIX))
		return NULL;
	/* libinput reads it already (a joystick-shaped tablet, say) */
	q.sysname = sysname;
	q.found = FALSE;
	gowl_input_config_foreach_device(st->compositor, check_owned, &q);
	if (q.found)
		return NULL;

	vendor = read_sysfs(sysname, "id/vendor");
	product = read_sysfs(sysname, "id/product");

	pad = g_new0(GowlRemapPad, 1);
	pad->compositor = st->compositor;
	pad->sysname = g_strdup(sysname);
	pad->devnode = g_build_filename(PAD_DEV_DIR, sysname, NULL);
	pad->fd = -1;
	memcpy(pad->keybits, keybits, sizeof keybits);
	memcpy(pad->absbits, absbits, sizeof absbits);
	pad->info = gowl_input_device_info_new(
		++st->compositor->remap_next_id, type,
		name != NULL ? name : sysname, sysname,
		vendor != NULL ? (guint16)g_ascii_strtoull(vendor, NULL, 16) : 0,
		product != NULL ? (guint16)g_ascii_strtoull(product, NULL, 16) : 0);
	gowl_input_remap_analog_init(&pad->analog);
	g_hash_table_insert(st->pads, pad->sysname, pad);
	return pad;
}

/* Every eventN under /sys/class/input. */
static void
pads_scan(GowlRemapPads *st)
{
	DIR *dir;
	struct dirent *de;

	dir = opendir(PAD_SYSFS_DIR);
	if (dir == NULL)
		return;
	while ((de = readdir(dir)) != NULL)
		pad_discover(st, de->d_name);
	closedir(dir);
}

/* --- opening and reading --- */

/* Re-reads every present axis (EVIOCGABS gives the current value too):
 * at open, and after the kernel dropped events. */
static void
pad_read_axes(GowlRemapPad *pad)
{
	guint code;

	for (code = 0; code < ABS_CNT; code++) {
		struct input_absinfo ai;
		PadAxis *a = &pad->axes[code];

		if (!bit_test(pad->absbits, code))
			continue;
		if (ioctl(pad->fd, EVIOCGABS(code), &ai) < 0)
			continue;
		a->present = TRUE;
		a->abs.min = ai.minimum;
		a->abs.max = ai.maximum;
		a->abs.flat = ai.flat;
		a->abs.fuzz = ai.fuzz;
		a->raw = ai.value;
		pad->dirty |= (guint64)1 << code;
	}
}

static int on_pad_readable(int fd, uint32_t mask, void *data);

/*
 * Opens the node: through the wlroots session when there is one (libseat
 * hands over an fd the seat owns, revoked when we lose the VT), else a
 * plain open() -- refused as root, where it would bypass every
 * permission the user's session would have applied.
 */
static gboolean
pad_open(GowlRemapPad *pad)
{
	GowlCompositor *self;
	GowlRemapPads *st;
	struct wl_event_loop *loop;
	int fd;
	int flags;

	if (pad->fd >= 0)
		return TRUE;
	self = pad->compositor;
	st = (GowlRemapPads *)self->remap_pads;

	fd = -1;
	if (self->session != NULL) {
		pad->wdev = wlr_session_open_file(self->session, pad->devnode);
		if (pad->wdev != NULL)
			fd = pad->wdev->fd;
	} else if (geteuid() == 0) {
		if (st != NULL && !st->warned_root) {
			g_message("input-remap: not opening game controllers as root "
			          "without a seat session");
			st->warned_root = TRUE;
		}
		return FALSE;
	} else {
		fd = open(pad->devnode, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	}
	if (fd < 0) {
		if (!pad->open_failed)
			g_message("input-remap: cannot open controller %s \"%s\": %s",
			          pad->devnode, pad->info->name,
			          g_strerror(errno != 0 ? errno : EACCES));
		pad->open_failed = TRUE;
		if (pad->wdev != NULL) {
			wlr_session_close_file(self->session, pad->wdev);
			pad->wdev = NULL;
		}
		return FALSE;
	}
	flags = fcntl(fd, F_GETFL);
	if (flags >= 0)
		fcntl(fd, F_SETFL, flags | O_NONBLOCK);
	pad->fd = fd;
	pad->open_failed = FALSE;
	memset(pad->axes, 0, sizeof pad->axes);
	pad->dirty = 0;
	pad_read_axes(pad);

	loop = gowl_compositor_get_event_loop(self);
	if (loop != NULL)
		pad->source = wl_event_loop_add_fd(loop, fd, WL_EVENT_READABLE,
		                                   on_pad_readable, pad);
	return TRUE;
}

static void
pad_close(GowlRemapPad *pad)
{
	if (pad->source != NULL) {
		wl_event_source_remove(pad->source);
		pad->source = NULL;
	}
	if (pad->fd >= 0 && pad->grabbed)
		ioctl(pad->fd, EVIOCGRAB, 0);
	pad->grabbed = FALSE;
	if (pad->wdev != NULL) {
		/* the session owns the fd */
		if (pad->compositor->session != NULL)
			wlr_session_close_file(pad->compositor->session, pad->wdev);
		pad->wdev = NULL;
	} else if (pad->fd >= 0) {
		close(pad->fd);
	}
	pad->fd = -1;
	memset(pad->axes, 0, sizeof pad->axes);
	pad->dirty = 0;
	pad->scroll_acc[0] = 0.0;
	pad->scroll_acc[1] = 0.0;
}

/* One edge of one input on @pad: through the remap core when claimed,
 * to identify mode only when merely observed. */
static void
pad_edge(
	GowlRemapPad *pad,
	guint32       code,
	gboolean      pressed,
	gdouble       value
){
	GowlInputRemapEvent ev;
	GowlRemapPads *st;

	memset(&ev, 0, sizeof ev);
	ev.kind = GOWL_INPUT_REMAP_EVENT_PAD;
	ev.code = code;
	ev.pressed = pressed;
	ev.time_msec = now_msec();
	ev.device_id = pad->info->id;
	ev.value = value;

	if (pad->claimed && pad->rd != NULL) {
		gowl_input_remap_core_source_edge(pad->rd, &ev);
		return;
	}
	st = (GowlRemapPads *)pad->compositor->remap_pads;
	if (st != NULL && st->identifying)
		gowl_input_remap_core_identify_pad(pad->compositor, pad->info, &ev);
}

/*
 * One axis after a SYN_REPORT: normalised, inverted, deadzoned; its two
 * directions pressed or released through the hysteresis; and, when the
 * axis drives a continuous target, the tick armed while it is deflected.
 */
static void
pad_axis_update(
	GowlRemapPad *pad,
	guint         code
){
	PadAxis *a;
	gdouble v;
	gdouble dz;
	guint dir;

	a = &pad->axes[code];
	if (!a->present)
		return;
	a->centred = axis_is_centred(code, &a->abs, pad->analog.centred);
	v = gowl_remap_analog_normalize(&a->abs, a->raw, a->centred);
	if ((pad->analog.invert >> code) & 1)
		v = a->centred ? -v : 1.0 - v;
	dz = pad->analog.deadzone >= 0.0
		? pad->analog.deadzone
		: gowl_remap_analog_default_deadzone(&a->abs, a->centred);
	/* a hat is -1/0/1 already, with nothing to filter */
	if (code >= ABS_HAT0X && code <= ABS_HAT3Y)
		dz = 0.0;
	v = gowl_remap_analog_deadzone(v, dz);
	a->value = v;

	for (dir = GOWL_INPUT_REMAP_ABS_POS; dir <= GOWL_INPUT_REMAP_ABS_NEG; dir++) {
		gdouble amount;
		gboolean now;

		amount = dir == GOWL_INPUT_REMAP_ABS_POS ? v : -v;
		now = gowl_remap_analog_hysteresis(a->down[dir], amount,
		                                   pad->analog.press,
		                                   pad->analog.release);
		if (now == a->down[dir])
			continue;
		a->down[dir] = now;
		pad_edge(pad, GOWL_INPUT_REMAP_ABS_INPUT(code, dir), now, amount);
	}

	if (v != 0.0) {
		if (a->deflected_since == 0)
			a->deflected_since = g_get_monotonic_time();
		if (pad->claimed && a->cont.kind != GOWL_INPUT_REMAP_TARGET_PASS)
			tick_arm((GowlRemapPads *)pad->compositor->remap_pads);
	} else {
		a->deflected_since = 0;
	}
}

/* After SYN_DROPPED: the kernel lost events, so re-read the truth --
 * every axis, and every button, releasing or pressing what changed. */
static void
pad_resync(GowlRemapPad *pad)
{
	unsigned long keys[PAD_NBITS(KEY_CNT)];
	guint code;

	pad_read_axes(pad);
	memset(keys, 0, sizeof keys);
	if (ioctl(pad->fd, EVIOCGKEY(sizeof keys), keys) < 0)
		return;
	for (code = BTN_MISC; code < KEY_CNT; code++) {
		gboolean down;
		gboolean held;

		if (!bit_test(pad->keybits, code))
			continue;
		down = bit_test(keys, code);
		held = pad->rd != NULL
		       && gowl_input_remap_core_source_has_held(pad->rd, code);
		if (down != held && pad->claimed)
			pad_edge(pad, code, down, 0.0);
	}
}

static int
on_pad_readable(
	int       fd,
	uint32_t  mask,
	void     *data
){
	GowlRemapPad *pad;
	struct input_event evs[64];
	ssize_t n;

	pad = (GowlRemapPad *)data;
	(void)fd;

	if (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)) {
		pad_close(pad);
		return 0;
	}
	for (;;) {
		gsize i;
		gsize count;

		n = read(pad->fd, evs, sizeof evs);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			if (errno != EAGAIN)
				/* unplugged, or revoked when the VT was left: the
				 * inotify delete or the session's active signal
				 * decides what happens next */
				pad_close(pad);
			return 0;
		}
		if (n == 0)
			return 0;
		count = (gsize)n / sizeof evs[0];
		for (i = 0; i < count; i++) {
			const struct input_event *e = &evs[i];

			if (pad->fd < 0)
				return 0;
			if (e->type == EV_SYN && e->code == SYN_DROPPED) {
				pad->dropped = TRUE;
				continue;
			}
			if (pad->dropped) {
				if (e->type == EV_SYN && e->code == SYN_REPORT) {
					pad->dropped = FALSE;
					pad_resync(pad);
				} else {
					continue;
				}
			}
			if (e->type == EV_KEY && e->code < KEY_CNT && e->value != 2) {
				pad_edge(pad, e->code, e->value != 0, 0.0);
			} else if (e->type == EV_ABS && e->code < ABS_CNT) {
				pad->axes[e->code].raw = e->value;
				pad->dirty |= (guint64)1 << e->code;
			} else if (e->type == EV_SYN && e->code == SYN_REPORT) {
				guint code;
				guint64 dirty;

				dirty = pad->dirty;
				pad->dirty = 0;
				for (code = 0; code < ABS_CNT && dirty != 0; code++) {
					if (!((dirty >> code) & 1))
						continue;
					dirty &= ~((guint64)1 << code);
					pad_axis_update(pad, code);
				}
			}
		}
		if ((gsize)n < sizeof evs)
			return 0;
	}
}

/* --- continuous targets --- */

static void
tick_arm(GowlRemapPads *st)
{
	if (st == NULL || st->tick == NULL || st->ticking)
		return;
	st->ticking = TRUE;
	st->last_tick_us = g_get_monotonic_time();
	wl_event_source_timer_update(st->tick, PAD_TICK_MS);
}

/*
 * The tick: every claimed pad's deflected continuous axes, turned into
 * pointer motion and scroll notches for the time since the last tick.
 * Re-arms only while something is still deflected, so a controller at
 * rest costs no timer at all.
 */
static int
on_tick(void *data)
{
	GowlRemapPads *st;
	GHashTableIter iter;
	gpointer value;
	gint64 now;
	gdouble dt;
	gdouble dx;
	gdouble dy;
	gboolean again;
	guint32 t;

	st = (GowlRemapPads *)data;
	now = g_get_monotonic_time();
	dt = (gdouble)(now - st->last_tick_us) / 1e6;
	if (dt > 0.1)
		dt = 0.1;            /* a stalled loop must not jump the pointer */
	st->last_tick_us = now;
	t = now_msec();
	dx = 0.0;
	dy = 0.0;
	again = FALSE;

	g_hash_table_iter_init(&iter, st->pads);
	while (g_hash_table_iter_next(&iter, NULL, &value)) {
		GowlRemapPad *pad = (GowlRemapPad *)value;
		guint code;

		if (!pad->claimed || pad->fd < 0)
			continue;
		for (code = 0; code < ABS_CNT; code++) {
			PadAxis *a = &pad->axes[code];
			gdouble held;
			gdouble speed;

			if (!a->present || a->value == 0.0
			    || a->cont.kind == GOWL_INPUT_REMAP_TARGET_PASS)
				continue;
			again = TRUE;
			held = a->deflected_since != 0
				? (gdouble)(now - a->deflected_since) / 1e6 : 0.0;
			if (a->cont.kind == GOWL_INPUT_REMAP_TARGET_POINTER) {
				speed = gowl_remap_analog_speed(a->value,
					pad->analog.pointer_curve, pad->analog.pointer_speed,
					pad->analog.pointer_accel, held);
				if (a->cont.component == GOWL_INPUT_REMAP_COMPONENT_Y)
					dy += speed * dt;
				else
					dx += speed * dt;
			} else if (a->cont.kind == GOWL_INPUT_REMAP_TARGET_SCROLL) {
				guint o;
				gdouble *acc;

				o = a->cont.component == GOWL_INPUT_REMAP_COMPONENT_X ? 1 : 0;
				acc = &pad->scroll_acc[o];
				*acc += gowl_remap_analog_curve(a->value,
					pad->analog.pointer_curve) * pad->analog.scroll_rate
					* 120.0 * dt;
				/* whole notches only, as a real wheel sends them */
				while (*acc >= 120.0 || *acc <= -120.0) {
					gint32 notch = *acc > 0.0 ? 120 : -120;

					*acc -= (gdouble)notch;
					gowl_compositor_remap_handle_axis(st->compositor,
						o == 1, (gdouble)notch / 8.0, notch, t);
				}
			}
		}
	}

	if (dx != 0.0 || dy != 0.0) {
		if (st->compositor->idle_mgr != NULL)
			gowl_idle_manager_note_activity(st->compositor->idle_mgr);
		gowl_compositor_remap_handle_motion(st->compositor, dx, dy, t);
	}
	if (again) {
		wl_event_source_timer_update(st->tick, PAD_TICK_MS);
	} else {
		st->ticking = FALSE;
		wl_event_source_timer_update(st->tick, 0);
	}
	return 0;
}

/* --- claiming --- */

/* What the remapper's rules say about @pad: the analog settings and grab
 * of the highest-priority rule that matches it (probed with input 0,
 * which no rule can mention), and each axis's continuous target. */
static void
pad_resolve(
	GowlRemapPad      *pad,
	GowlInputRemapper *r
){
	GowlInputRemapEvent ev;
	const GowlInputRemapTarget *target;
	g_autoptr(GowlInputRemapRule) top = NULL;
	guint code;

	memset(&ev, 0, sizeof ev);
	ev.kind = GOWL_INPUT_REMAP_EVENT_PAD;
	ev.device_id = pad->info->id;
	ev.pressed = FALSE;    /* a probe: never logged as a press */
	target = NULL;
	top = gowl_input_remapper_map_event(r, pad->info, &ev, &target);
	if (top != NULL) {
		gowl_input_remap_rule_get_analog(top, &pad->analog);
		pad->want_grab = gowl_input_remap_rule_get_grab(top);
	} else {
		gowl_input_remap_analog_init(&pad->analog);
		pad->want_grab = FALSE;
	}

	for (code = 0; code < ABS_CNT; code++) {
		PadAxis *a = &pad->axes[code];
		g_autoptr(GowlInputRemapRule) rule = NULL;

		a->cont.kind = GOWL_INPUT_REMAP_TARGET_PASS;
		if (!bit_test(pad->absbits, code))
			continue;
		ev.code = GOWL_INPUT_REMAP_ABS_INPUT(code, GOWL_INPUT_REMAP_ABS_WHOLE);
		target = NULL;
		rule = gowl_input_remapper_map_event(r, pad->info, &ev, &target);
		if (rule == NULL || target == NULL)
			continue;
		if (gowl_input_remap_target_get_kind(target) == GOWL_INPUT_REMAP_TARGET_POINTER
		    || gowl_input_remap_target_get_kind(target) == GOWL_INPUT_REMAP_TARGET_SCROLL) {
			a->cont.kind = gowl_input_remap_target_get_kind(target);
			a->cont.component = gowl_input_remap_target_get_component(target);
		}
	}
}

/* Whether @pad should be open: claimed, or being observed for identify
 * mode.  The only way to reach pad_open(). */
static gboolean
want_open(GowlRemapPad *pad)
{
	GowlRemapPads *st;

	st = (GowlRemapPads *)pad->compositor->remap_pads;
	return pad->claimed || (st != NULL && st->identifying);
}

/*
 * Re-decides one pad against the remapper (NULL: none, release it), and
 * opens, grabs, ungrabs or closes it to match.
 */
static void
pad_apply(
	GowlRemapPads     *st,
	GowlRemapPad      *pad,
	GowlInputRemapper *r
){
	gboolean want;
	gboolean was;
	guint code;

	(void)st;
	was = pad->claimed;
	want = r != NULL && gowl_input_remapper_claims_device(r, pad->info);

	if (want && !was) {
		pad->claimed = TRUE;
		pad->info->claimed = TRUE;
		pad->rd = gowl_input_remap_core_source_new(pad->compositor,
			gowl_input_device_info_copy(pad->info));
	} else if (!want && was) {
		/* Releases whatever it held, each release mirroring its press */
		g_clear_pointer(&pad->rd, gowl_input_remap_core_source_free);
		pad->claimed = FALSE;
		pad->info->claimed = FALSE;
		for (code = 0; code < ABS_CNT; code++) {
			pad->axes[code].down[GOWL_INPUT_REMAP_ABS_POS] = FALSE;
			pad->axes[code].down[GOWL_INPUT_REMAP_ABS_NEG] = FALSE;
		}
	}

	if (want_open(pad))
		pad_open(pad);
	else
		pad_close(pad);

	if (pad->claimed && r != NULL)
		pad_resolve(pad, r);
	else
		pad->want_grab = FALSE;

	/* The grab follows the deciding rule, and only a claimed pad is
	 * ever grabbed: identify mode merely watches. */
	if (pad->fd >= 0 && pad->claimed && pad->want_grab && !pad->grabbed) {
		if (ioctl(pad->fd, EVIOCGRAB, 1) == 0)
			pad->grabbed = TRUE;
		else
			g_message("input-remap: cannot grab %s \"%s\": %s",
			          pad->devnode, pad->info->name, g_strerror(errno));
	} else if (pad->fd >= 0 && pad->grabbed
	           && (!pad->claimed || !pad->want_grab)) {
		ioctl(pad->fd, EVIOCGRAB, 0);
		pad->grabbed = FALSE;
	}

	/* A stick already deflected when the rules changed starts moving */
	if (pad->claimed && pad->fd >= 0) {
		for (code = 0; code < ABS_CNT; code++)
			if (pad->axes[code].present)
				pad->dirty |= (guint64)1 << code;
		for (code = 0; code < ABS_CNT; code++)
			if ((pad->dirty >> code) & 1)
				pad_axis_update(pad, code);
		pad->dirty = 0;
	}

	if (want != was && r != NULL)
		gowl_input_remapper_device_changed(r, pad->info, want);
}

/* inotify on /dev/input: a node came, went, or had its ACL set. */
static int
on_inotify(
	int       fd,
	uint32_t  mask,
	void     *data
){
	GowlRemapPads *st;
	GowlInputRemapper *r;
	gchar buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
	ssize_t n;
	gchar *p;

	(void)mask;
	st = (GowlRemapPads *)data;
	r = active_remapper(st->compositor);
	for (;;) {
		n = read(fd, buf, sizeof buf);
		if (n <= 0)
			return 0;
		for (p = buf; p < buf + n;
		     p += sizeof(struct inotify_event) + ((struct inotify_event *)(gpointer)p)->len) {
			struct inotify_event *ie = (struct inotify_event *)(gpointer)p;
			GowlRemapPad *pad;

			if (ie->len == 0 || !g_str_has_prefix(ie->name, "event"))
				continue;
			if (ie->mask & IN_DELETE) {
				pad = g_hash_table_lookup(st->pads, ie->name);
				if (pad != NULL) {
					gboolean was = pad->claimed;
					GowlInputDeviceInfo *info;

					info = gowl_input_device_info_copy(pad->info);
					info->claimed = FALSE;
					g_hash_table_remove(st->pads, ie->name);
					if (was && r != NULL)
						gowl_input_remapper_device_changed(r, info, FALSE);
					gowl_input_device_info_free(info);
				}
				continue;
			}
			pad = pad_discover(st, ie->name);
			if (pad == NULL)
				continue;
			/* a new node, or its permissions changed: decide again
			 * (an earlier EACCES may now be an fd) */
			if (pad->fd < 0)
				pad_apply(st, pad, r);
		}
	}
}

/* --- entry points --- */

/**
 * gowl_input_remap_pads_reevaluate: (skip)
 * @self: the compositor
 *
 * Re-decides every game controller against the active remapper.  With
 * none active and nothing ever looked at, returns at once; with none
 * active after there was one, releases and closes everything and frees
 * the state.
 */
void
gowl_input_remap_pads_reevaluate(GowlCompositor *self)
{
	GowlInputRemapper *r;
	GowlRemapPads *st;
	GHashTableIter iter;
	gpointer value;

	r = active_remapper(self);
	if (r == NULL && self->remap_pads == NULL)
		return;
	if (r == NULL) {
		st = (GowlRemapPads *)self->remap_pads;
		if (st->identifying)
			return;          /* identify mode still wants them open */
		g_hash_table_iter_init(&iter, st->pads);
		while (g_hash_table_iter_next(&iter, NULL, &value))
			pad_apply(st, (GowlRemapPad *)value, NULL);
		pads_destroy(self);
		return;
	}

	st = pads_state(self);
	pads_scan(st);
	g_hash_table_iter_init(&iter, st->pads);
	while (g_hash_table_iter_next(&iter, NULL, &value))
		pad_apply(st, (GowlRemapPad *)value, r);
}

/**
 * gowl_input_remap_pads_list: (skip)
 * @self: the compositor
 * @out: (element-type GowlInputDeviceInfo): appended to
 *
 * Every game controller seen, claimed or not.  Controllers are looked
 * for only once a remapper has been active.
 */
void
gowl_input_remap_pads_list(
	GowlCompositor *self,
	GPtrArray      *out
){
	GowlRemapPads *st;
	GHashTableIter iter;
	gpointer value;

	st = (GowlRemapPads *)self->remap_pads;
	if (st == NULL)
		return;
	pads_scan(st);
	g_hash_table_iter_init(&iter, st->pads);
	while (g_hash_table_iter_next(&iter, NULL, &value))
		g_ptr_array_add(out, gowl_input_device_info_copy(
			((GowlRemapPad *)value)->info));
}

/**
 * gowl_input_remap_pads_identify: (skip)
 * @self: the compositor
 * @on: whether identify mode starts or stops
 *
 * While on, every controller is open read-only -- never grabbed -- so a
 * press on one no rule names yet is reported.  Off closes the ones no
 * rule claims.
 */
void
gowl_input_remap_pads_identify(
	GowlCompositor *self,
	gboolean        on
){
	GowlRemapPads *st;
	GHashTableIter iter;
	gpointer value;
	GowlInputRemapper *r;

	r = active_remapper(self);
	if (on) {
		if (r == NULL && self->remap_pads == NULL)
			return;
		st = pads_state(self);
		st->identifying = TRUE;
		pads_scan(st);
	} else {
		st = (GowlRemapPads *)self->remap_pads;
		if (st == NULL || !st->identifying)
			return;
		st->identifying = FALSE;
	}
	g_hash_table_iter_init(&iter, st->pads);
	while (g_hash_table_iter_next(&iter, NULL, &value))
		pad_apply(st, (GowlRemapPad *)value, r);
	if (!on && r == NULL)
		pads_destroy(self);
}

/**
 * gowl_input_remap_pads_describe: (skip)
 * @self: the compositor
 * @id: a device id
 *
 * Returns: (transfer full) (nullable): the controller's axes and buttons
 *   as JSON, or %NULL when @id is not a controller
 */
gchar *
gowl_input_remap_pads_describe(
	GowlCompositor *self,
	guint           id
){
	GowlRemapPads *st;
	GHashTableIter iter;
	gpointer value;
	GowlRemapPad *pad;
	g_autoptr(JsonBuilder) b = NULL;
	g_autoptr(JsonGenerator) gen = NULL;
	g_autoptr(JsonNode) root = NULL;
	guint code;

	st = (GowlRemapPads *)self->remap_pads;
	if (st == NULL)
		return NULL;
	pad = NULL;
	g_hash_table_iter_init(&iter, st->pads);
	while (g_hash_table_iter_next(&iter, NULL, &value))
		if (((GowlRemapPad *)value)->info->id == id)
			pad = (GowlRemapPad *)value;
	if (pad == NULL)
		return NULL;

	b = json_builder_new();
	json_builder_begin_object(b);
	json_builder_set_member_name(b, "open");
	json_builder_add_boolean_value(b, pad->fd >= 0);
	json_builder_set_member_name(b, "grabbed");
	json_builder_add_boolean_value(b, pad->grabbed);
	json_builder_set_member_name(b, "axes");
	json_builder_begin_array(b);
	for (code = 0; code < ABS_CNT; code++) {
		PadAxis *a = &pad->axes[code];
		g_autofree gchar *name = NULL;

		if (!bit_test(pad->absbits, code))
			continue;
		name = gowl_input_remap_code_to_name(
			GOWL_INPUT_REMAP_ABS_INPUT(code, GOWL_INPUT_REMAP_ABS_WHOLE));
		json_builder_begin_object(b);
		json_builder_set_member_name(b, "name");
		json_builder_add_string_value(b, name);
		if (a->present) {
			json_builder_set_member_name(b, "min");
			json_builder_add_int_value(b, a->abs.min);
			json_builder_set_member_name(b, "max");
			json_builder_add_int_value(b, a->abs.max);
			json_builder_set_member_name(b, "flat");
			json_builder_add_int_value(b, a->abs.flat);
			json_builder_set_member_name(b, "fuzz");
			json_builder_add_int_value(b, a->abs.fuzz);
			json_builder_set_member_name(b, "centred");
			json_builder_add_boolean_value(b,
				axis_is_centred(code, &a->abs, pad->analog.centred));
			json_builder_set_member_name(b, "value");
			json_builder_add_double_value(b, a->value);
		}
		json_builder_end_object(b);
	}
	json_builder_end_array(b);
	json_builder_set_member_name(b, "buttons");
	json_builder_begin_array(b);
	for (code = BTN_MISC; code < KEY_CNT; code++) {
		g_autofree gchar *name = NULL;

		if (!bit_test(pad->keybits, code))
			continue;
		name = gowl_input_remap_code_to_name(code);
		json_builder_add_string_value(b, name);
	}
	json_builder_end_array(b);
	json_builder_end_object(b);

	root = json_builder_get_root(b);
	gen = json_generator_new();
	json_generator_set_root(gen, root);
	return json_generator_to_data(gen, NULL);
}

/**
 * gowl_input_remap_pads_finish: (skip)
 * @self: the compositor, during teardown
 *
 * Every controller released (its outputs mirrored), ungrabbed and
 * closed; the watch, the tick and the session listener removed.
 */
void
gowl_input_remap_pads_finish(GowlCompositor *self)
{
	pads_destroy(self);
}

#else /* !GOWL_HAVE_EVDEV_PADS */

/*
 * A build without <linux/input.h>, <linux/uinput.h> and inotify (the
 * probe in config.mk): game controllers are simply not a remap source.
 * Keyboards and pointers are unaffected.
 */

void
gowl_input_remap_pads_reevaluate(GowlCompositor *self)
{
	(void)self;
}

void
gowl_input_remap_pads_list(
	GowlCompositor *self,
	GPtrArray      *out
){
	(void)self;
	(void)out;
}

void
gowl_input_remap_pads_identify(
	GowlCompositor *self,
	gboolean        on
){
	(void)self;
	(void)on;
}

gchar *
gowl_input_remap_pads_describe(
	GowlCompositor *self,
	guint           id
){
	(void)self;
	(void)id;
	return NULL;
}

void
gowl_input_remap_pads_finish(GowlCompositor *self)
{
	(void)self;
}

#endif /* GOWL_HAVE_EVDEV_PADS */
