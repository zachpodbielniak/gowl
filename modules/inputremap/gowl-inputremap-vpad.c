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
 * gowl-inputremap-vpad.c - Virtual gamepads through /dev/uinput.
 *
 * A pad's state is never stored as "the stick is at x"; it is DERIVED,
 * every time anything changes, from what is held:
 *
 *   - each held key/button/direction mapped to the pad is a HOLD, keyed
 *     by its target (the core's held table guarantees the release names
 *     the same target as the press, and keeps the rule -- and so the
 *     target -- alive until then);
 *   - a stick following the mouse adds its own deflection, which a
 *     wl_event_loop timer returns to centre once the mouse is still.
 *
 * So two keys on opposite directions cancel, W+D is up-right on the
 * unit circle (gowl_remap_analog_combine), and releasing one of two
 * keys holding the same button keeps it held.  No timer runs for keys.
 *
 * No /dev/uinput access: the rule still loads; the first use logs the
 * reason once per pad, the module status carries it, and nothing else
 * happens.
 */

#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "gowl-inputremap"

#include "gowl-inputremap-vpad.h"
#include "boxed/gowl-input-remap-analog.h"

#ifdef GOWL_HAVE_EVDEV_PADS

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/input.h>
#include <linux/uinput.h>

#define VPAD_STICK_MIN   (-32768)
#define VPAD_STICK_MAX   (32767)
#define VPAD_TRIGGER_MAX (255)

/* One held input on a pad. */
typedef struct {
	GowlInputRemapTargetKind kind;
	guint                    side;
	guint                    component;
	gdouble                  value;
	guint32                  code;
	guint                    count;    /* two devices may hold one target */
} VpadHold;

typedef struct _Vpad Vpad;

/* The decay timer of one stick following the mouse. */
typedef struct {
	Vpad                   *pad;
	guint                   side;
	struct wl_event_source *timer;
} VpadDecay;

struct _Vpad {
	GowlInputRemapVpads *owner;
	gchar               *name;
	guint16              vendor;
	guint16              product;
	GArray              *buttons;            /* guint32 */
	GArray              *axes;               /* guint32 */
	int                  fd;                 /* -1 until created */
	gboolean             failed;             /* creation failed: logged once */
	GHashTable          *holds;              /* target -> VpadHold* */
	gdouble              follow[2][2];       /* [side][component] */
	VpadDecay            decay[2];
	gint32               sent_abs[ABS_CNT];
	gboolean             sent_key[KEY_CNT];
	gboolean             sent_any;
};

struct _GowlInputRemapVpads {
	GHashTable           *pads;              /* name -> Vpad* */
	struct wl_event_loop *loop;
	gchar                *error;
};

static void
vpad_destroy_device(Vpad *pad)
{
	guint i;

	for (i = 0; i < 2; i++) {
		if (pad->decay[i].timer != NULL) {
			wl_event_source_remove(pad->decay[i].timer);
			pad->decay[i].timer = NULL;
		}
	}
	if (pad->fd >= 0) {
		ioctl(pad->fd, UI_DEV_DESTROY);
		close(pad->fd);
		pad->fd = -1;
	}
}

static void
vpad_free(gpointer data)
{
	Vpad *pad = (Vpad *)data;

	if (pad == NULL)
		return;
	vpad_destroy_device(pad);
	g_hash_table_destroy(pad->holds);
	g_clear_pointer(&pad->buttons, g_array_unref);
	g_clear_pointer(&pad->axes, g_array_unref);
	g_free(pad->name);
	g_free(pad);
}

/**
 * gowl_inputremap_vpads_available: (skip)
 *
 * Returns: whether this build can make virtual pads at all
 */
gboolean
gowl_inputremap_vpads_available(void)
{
	return TRUE;
}

/**
 * gowl_inputremap_vpads_new: (skip)
 *
 * Returns: (transfer full): an empty set of virtual pads
 */
GowlInputRemapVpads *
gowl_inputremap_vpads_new(void)
{
	GowlInputRemapVpads *self;

	self = g_new0(GowlInputRemapVpads, 1);
	self->pads = g_hash_table_new_full(g_str_hash, g_str_equal, NULL,
	                                   vpad_free);
	return self;
}

/**
 * gowl_inputremap_vpads_free: (skip)
 * @self: (nullable): the pads
 *
 * Destroys every uinput device and frees @self.
 */
void
gowl_inputremap_vpads_free(GowlInputRemapVpads *self)
{
	if (self == NULL)
		return;
	g_hash_table_destroy(self->pads);
	g_free(self->error);
	g_free(self);
}

/**
 * gowl_inputremap_vpads_set_loop: (skip)
 * @self: the pads
 * @loop: (nullable): the compositor's event loop, for the mouse-stick
 *   decay timers; %NULL when the display goes (every timer is removed)
 */
void
gowl_inputremap_vpads_set_loop(
	GowlInputRemapVpads  *self,
	struct wl_event_loop *loop
){
	GHashTableIter iter;
	gpointer value;
	guint i;

	if (loop == NULL) {
		g_hash_table_iter_init(&iter, self->pads);
		while (g_hash_table_iter_next(&iter, NULL, &value)) {
			Vpad *pad = (Vpad *)value;

			for (i = 0; i < 2; i++) {
				if (pad->decay[i].timer != NULL) {
					wl_event_source_remove(pad->decay[i].timer);
					pad->decay[i].timer = NULL;
				}
			}
		}
	}
	self->loop = loop;
}

/* Creates the uinput device.  FALSE (with the reason recorded once) when
 * /dev/uinput cannot be opened or refuses the device. */
static gboolean
vpad_create(Vpad *pad)
{
	struct uinput_setup setup;
	g_autofree gchar *full = NULL;
	const gchar *step;
	int fd;
	guint i;

	if (pad->fd >= 0)
		return TRUE;
	if (pad->failed)
		return FALSE;

	step = "open /dev/uinput";
	fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0)
		goto fail;

	step = "declare the pad's buttons";
	if (ioctl(fd, UI_SET_EVBIT, EV_KEY) < 0)
		goto fail;
	for (i = 0; i < pad->buttons->len; i++)
		if (ioctl(fd, UI_SET_KEYBIT, g_array_index(pad->buttons, guint32, i)) < 0)
			goto fail;

	step = "declare the pad's axes";
	if (ioctl(fd, UI_SET_EVBIT, EV_ABS) < 0)
		goto fail;
	for (i = 0; i < pad->axes->len; i++) {
		struct uinput_abs_setup abs;
		guint32 code = g_array_index(pad->axes, guint32, i);

		memset(&abs, 0, sizeof abs);
		abs.code = (__u16)code;
		if (code >= ABS_HAT0X && code <= ABS_HAT3Y) {
			abs.absinfo.minimum = -1;
			abs.absinfo.maximum = 1;
		} else if (code == ABS_Z || code == ABS_RZ || code == ABS_GAS
		           || code == ABS_BRAKE || code == ABS_THROTTLE) {
			abs.absinfo.minimum = 0;
			abs.absinfo.maximum = VPAD_TRIGGER_MAX;
		} else {
			abs.absinfo.minimum = VPAD_STICK_MIN;
			abs.absinfo.maximum = VPAD_STICK_MAX;
		}
		if (ioctl(fd, UI_SET_ABSBIT, code) < 0
		    || ioctl(fd, UI_ABS_SETUP, &abs) < 0)
			goto fail;
	}

	step = "create the device";
	memset(&setup, 0, sizeof setup);
	full = g_strconcat(GOWL_INPUT_REMAP_PAD_NAME_PREFIX, pad->name, NULL);
	g_strlcpy(setup.name, full, sizeof setup.name);
	setup.id.bustype = BUS_VIRTUAL;
	setup.id.vendor = pad->vendor;
	setup.id.product = pad->product;
	setup.id.version = 1;
	if (ioctl(fd, UI_DEV_SETUP, &setup) < 0 || ioctl(fd, UI_DEV_CREATE) < 0)
		goto fail;

	pad->fd = fd;
	memset(pad->sent_abs, 0, sizeof pad->sent_abs);
	memset(pad->sent_key, 0, sizeof pad->sent_key);
	pad->sent_any = FALSE;
	g_message("inputremap: created virtual pad \"%s\" (%04x:%04x)", full,
	          (guint)pad->vendor, (guint)pad->product);
	return TRUE;

fail:
	{
		gint err = errno;

		if (fd >= 0)
			close(fd);
		pad->failed = TRUE;
		g_free(pad->owner->error);
		pad->owner->error = g_strdup_printf(
			"virtual pad %s: cannot %s: %s%s", pad->name, step,
			g_strerror(err),
			err == EACCES || err == EPERM
			? " (needs write access to /dev/uinput: a uaccess rule, "
			  "or the input group)" : "");
		g_warning("inputremap: %s", pad->owner->error);
		return FALSE;
	}
}

/* The pad a rule declares, made on first sight (not yet created as a
 * device: that waits for the first press). */
static Vpad *
vpad_for_rule(
	GowlInputRemapVpads *self,
	GowlInputRemapRule  *rule
){
	const gchar *name;
	Vpad *pad;

	name = gowl_input_remap_rule_get_virtual_pad(rule);
	if (name == NULL)
		return NULL;
	pad = g_hash_table_lookup(self->pads, name);
	if (pad != NULL)
		return pad;

	pad = g_new0(Vpad, 1);
	pad->owner = self;
	pad->name = g_strdup(name);
	pad->vendor = gowl_input_remap_rule_get_virtual_pad_vendor(rule);
	pad->product = gowl_input_remap_rule_get_virtual_pad_product(rule);
	pad->buttons = gowl_input_remap_rule_get_virtual_pad_buttons(rule);
	pad->axes = gowl_input_remap_rule_get_virtual_pad_axes(rule);
	pad->fd = -1;
	pad->holds = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL,
	                                   g_free);
	pad->decay[0].pad = pad;
	pad->decay[0].side = GOWL_INPUT_REMAP_SIDE_LEFT;
	pad->decay[1].pad = pad;
	pad->decay[1].side = GOWL_INPUT_REMAP_SIDE_RIGHT;
	g_hash_table_insert(self->pads, pad->name, pad);
	return pad;
}

static gboolean
pad_has(
	GArray  *codes,
	guint32  code
){
	guint i;

	for (i = 0; i < codes->len; i++)
		if (g_array_index(codes, guint32, i) == code)
			return TRUE;
	return FALSE;
}

static void
emit(
	Vpad    *pad,
	guint16  type,
	guint16  code,
	gint32   value
){
	struct input_event ev;

	memset(&ev, 0, sizeof ev);
	ev.type = type;
	ev.code = code;
	ev.value = value;
	if (write(pad->fd, &ev, sizeof ev) < 0 && errno != EAGAIN)
		g_debug("inputremap: virtual pad %s: write: %s", pad->name,
		        g_strerror(errno));
}

/* One axis value, written only when it changed. */
static gboolean
emit_abs(
	Vpad    *pad,
	guint32  code,
	gint32   raw
){
	if (!pad_has(pad->axes, code))
		return FALSE;
	if (pad->sent_any && pad->sent_abs[code] == raw)
		return FALSE;
	pad->sent_abs[code] = raw;
	emit(pad, EV_ABS, (guint16)code, raw);
	return TRUE;
}

/*
 * Recomputes the whole pad from its holds and the mouse deflection,
 * writes what changed, and closes the report.
 */
static void
vpad_flush(Vpad *pad)
{
	gdouble pos[3][2];
	gdouble neg[3][2];
	gdouble trig[2];
	gboolean keys_down[KEY_CNT];
	GHashTableIter iter;
	gpointer value;
	gboolean changed;
	guint s;
	guint i;

	if (pad->fd < 0)
		return;
	memset(pos, 0, sizeof pos);
	memset(neg, 0, sizeof neg);
	memset(trig, 0, sizeof trig);
	memset(keys_down, 0, sizeof keys_down);

	g_hash_table_iter_init(&iter, pad->holds);
	while (g_hash_table_iter_next(&iter, NULL, &value)) {
		VpadHold *h = (VpadHold *)value;

		switch (h->kind) {
		case GOWL_INPUT_REMAP_TARGET_STICK:
			if (h->value > 0.0)
				pos[h->side][h->component] = MAX(pos[h->side][h->component],
				                                 h->value);
			else
				neg[h->side][h->component] = MAX(neg[h->side][h->component],
				                                 -h->value);
			break;
		case GOWL_INPUT_REMAP_TARGET_TRIGGER:
			trig[h->side] = MAX(trig[h->side], h->value);
			break;
		case GOWL_INPUT_REMAP_TARGET_PAD_BUTTON:
			if (h->code < KEY_CNT)
				keys_down[h->code] = TRUE;
			break;
		default:
			break;
		}
	}

	changed = FALSE;
	for (s = 0; s < 3; s++) {
		gdouble x;
		gdouble y;
		guint32 ax;
		guint32 ay;

		gowl_remap_analog_combine(pos[s][0], neg[s][0], pos[s][1], neg[s][1],
		                          &x, &y);
		if (s == GOWL_INPUT_REMAP_SIDE_DPAD) {
			/* a hat is -1, 0 or 1 */
			changed |= emit_abs(pad, ABS_HAT0X, x > 0.25 ? 1 : x < -0.25 ? -1 : 0);
			changed |= emit_abs(pad, ABS_HAT0Y, y > 0.25 ? 1 : y < -0.25 ? -1 : 0);
			continue;
		}
		x = CLAMP(x + pad->follow[s][0], -1.0, 1.0);
		y = CLAMP(y + pad->follow[s][1], -1.0, 1.0);
		ax = s == GOWL_INPUT_REMAP_SIDE_RIGHT ? ABS_RX : ABS_X;
		ay = s == GOWL_INPUT_REMAP_SIDE_RIGHT ? ABS_RY : ABS_Y;
		changed |= emit_abs(pad, ax,
			gowl_remap_analog_to_raw(x, VPAD_STICK_MIN, VPAD_STICK_MAX));
		changed |= emit_abs(pad, ay,
			gowl_remap_analog_to_raw(y, VPAD_STICK_MIN, VPAD_STICK_MAX));
	}
	changed |= emit_abs(pad, ABS_Z,
		gowl_remap_analog_to_raw(trig[0], 0, VPAD_TRIGGER_MAX));
	changed |= emit_abs(pad, ABS_RZ,
		gowl_remap_analog_to_raw(trig[1], 0, VPAD_TRIGGER_MAX));

	for (i = 0; i < pad->buttons->len; i++) {
		guint32 code = g_array_index(pad->buttons, guint32, i);

		if (code >= KEY_CNT || pad->sent_key[code] == keys_down[code])
			continue;
		pad->sent_key[code] = keys_down[code];
		emit(pad, EV_KEY, (guint16)code, keys_down[code] ? 1 : 0);
		changed = TRUE;
	}
	pad->sent_any = TRUE;
	if (changed)
		emit(pad, EV_SYN, SYN_REPORT, 0);
}

/* The mouse stopped: its stick goes back to centre. */
static int
on_decay(void *data)
{
	VpadDecay *d = (VpadDecay *)data;

	d->pad->follow[d->side][0] = 0.0;
	d->pad->follow[d->side][1] = 0.0;
	vpad_flush(d->pad);
	return 0;
}

/**
 * gowl_inputremap_vpads_render: (skip)
 * @self: the pads
 * @rule: the rule that decided the input, which declares the pad
 * @target: a stick, trigger or pad-button target of @rule
 * @event: the input
 * @amount: 1 for a press, 0 for its release; pixels for motion
 *
 * Applies one input to its pad, creating the device on first use.
 *
 * Returns: %TRUE when a device was driven
 */
gboolean
gowl_inputremap_vpads_render(
	GowlInputRemapVpads        *self,
	GowlInputRemapRule         *rule,
	const GowlInputRemapTarget *target,
	const GowlInputRemapEvent  *event,
	gdouble                     amount
){
	Vpad *pad;
	VpadHold *h;
	gpointer key;

	pad = vpad_for_rule(self, rule);
	if (pad == NULL)
		return FALSE;
	key = (gpointer)target;

	/* A stick following the mouse: deflection from this motion, back to
	 * centre once the mouse has been still for mouse-idle-ms. */
	if (event->kind == GOWL_INPUT_REMAP_EVENT_MOTION
	    && gowl_input_remap_target_is_follow(target)) {
		GowlInputRemapAnalog a;
		guint side;
		guint comp;

		if (!vpad_create(pad))
			return FALSE;
		gowl_input_remap_rule_get_analog(rule, &a);
		side = MIN(gowl_input_remap_target_get_side(target), 1u);
		comp = MIN(gowl_input_remap_target_get_component(target), 1u);
		pad->follow[side][comp] = CLAMP(amount * a.mouse_gain, -1.0, 1.0);
		if (pad->decay[side].timer == NULL && self->loop != NULL)
			pad->decay[side].timer = wl_event_loop_add_timer(self->loop,
				on_decay, &pad->decay[side]);
		if (pad->decay[side].timer != NULL)
			wl_event_source_timer_update(pad->decay[side].timer,
			                             (gint)a.mouse_idle_ms);
		vpad_flush(pad);
		return TRUE;
	}

	if (amount > 0.0) {
		/* a press: take the hold, then make the device if need be */
		if (!vpad_create(pad))
			return FALSE;
		h = g_hash_table_lookup(pad->holds, key);
		if (h == NULL) {
			h = g_new0(VpadHold, 1);
			h->kind = gowl_input_remap_target_get_kind(target);
			h->side = MIN(gowl_input_remap_target_get_side(target), 2u);
			h->component = MIN(gowl_input_remap_target_get_component(target), 1u);
			h->value = gowl_input_remap_target_get_value(target);
			h->code = gowl_input_remap_target_get_code(target);
			g_hash_table_insert(pad->holds, key, h);
		}
		h->count++;
	} else {
		/* a release lets go of exactly what its press took */
		h = g_hash_table_lookup(pad->holds, key);
		if (h == NULL)
			return FALSE;
		if (--h->count == 0)
			g_hash_table_remove(pad->holds, key);
	}
	vpad_flush(pad);
	return pad->fd >= 0;
}

/**
 * gowl_inputremap_vpads_sync: (skip)
 * @self: the pads
 * @rules: (element-type GowlInputRemapRule): every rule in force
 *
 * Destroys every pad no rule in @rules declares any more.  A pad whose
 * declaring rules all went away cannot be driven again, and a game
 * should see it unplugged rather than frozen.
 */
void
gowl_inputremap_vpads_sync(
	GowlInputRemapVpads *self,
	GPtrArray           *rules
){
	GHashTableIter iter;
	gpointer value;

	g_hash_table_iter_init(&iter, self->pads);
	while (g_hash_table_iter_next(&iter, NULL, &value)) {
		Vpad *pad = (Vpad *)value;
		gboolean declared;
		guint i;

		declared = FALSE;
		for (i = 0; rules != NULL && i < rules->len; i++) {
			const gchar *n = gowl_input_remap_rule_get_virtual_pad(
				g_ptr_array_index(rules, i));

			if (g_strcmp0(n, pad->name) == 0)
				declared = TRUE;
		}
		if (!declared) {
			if (pad->fd >= 0)
				g_message("inputremap: destroyed virtual pad \"%s%s\"",
				          GOWL_INPUT_REMAP_PAD_NAME_PREFIX, pad->name);
			g_hash_table_iter_remove(&iter);
		}
	}
}

/**
 * gowl_inputremap_vpads_destroy_all: (skip)
 * @self: the pads
 *
 * Every uinput device destroyed: the module was disabled or is going.
 */
void
gowl_inputremap_vpads_destroy_all(GowlInputRemapVpads *self)
{
	g_hash_table_remove_all(self->pads);
	g_clear_pointer(&self->error, g_free);
}

/**
 * gowl_inputremap_vpads_count: (skip)
 * @self: the pads
 *
 * Returns: how many uinput devices exist right now
 */
guint
gowl_inputremap_vpads_count(GowlInputRemapVpads *self)
{
	GHashTableIter iter;
	gpointer value;
	guint n;

	n = 0;
	g_hash_table_iter_init(&iter, self->pads);
	while (g_hash_table_iter_next(&iter, NULL, &value))
		if (((Vpad *)value)->fd >= 0)
			n++;
	return n;
}

/**
 * gowl_inputremap_vpads_get_error: (skip)
 * @self: the pads
 *
 * Returns: (nullable): why the last pad could not be created
 */
const gchar *
gowl_inputremap_vpads_get_error(GowlInputRemapVpads *self)
{
	return self->error;
}

/**
 * gowl_inputremap_vpads_to_json: (skip)
 * @self: the pads
 * @b: a builder positioned where an array value goes
 *
 * `[{name, device, id, created, holds, failed}, ...]'.
 */
void
gowl_inputremap_vpads_to_json(
	GowlInputRemapVpads *self,
	JsonBuilder         *b
){
	GHashTableIter iter;
	gpointer value;

	json_builder_begin_array(b);
	g_hash_table_iter_init(&iter, self->pads);
	while (g_hash_table_iter_next(&iter, NULL, &value)) {
		Vpad *pad = (Vpad *)value;
		g_autofree gchar *device = NULL;
		g_autofree gchar *id = NULL;

		device = g_strconcat(GOWL_INPUT_REMAP_PAD_NAME_PREFIX, pad->name, NULL);
		id = g_strdup_printf("%04x:%04x", (guint)pad->vendor,
		                     (guint)pad->product);
		json_builder_begin_object(b);
		json_builder_set_member_name(b, "name");
		json_builder_add_string_value(b, pad->name);
		json_builder_set_member_name(b, "device");
		json_builder_add_string_value(b, device);
		json_builder_set_member_name(b, "vendor-product");
		json_builder_add_string_value(b, id);
		json_builder_set_member_name(b, "created");
		json_builder_add_boolean_value(b, pad->fd >= 0);
		json_builder_set_member_name(b, "failed");
		json_builder_add_boolean_value(b, pad->failed);
		json_builder_set_member_name(b, "holds");
		json_builder_add_int_value(b, g_hash_table_size(pad->holds));
		json_builder_end_object(b);
	}
	json_builder_end_array(b);
}

#else /* !GOWL_HAVE_EVDEV_PADS */

/* No uinput on this system: pad targets load, and report why they do
 * nothing. */

struct _GowlInputRemapVpads {
	gchar *error;
};

gboolean
gowl_inputremap_vpads_available(void)
{
	return FALSE;
}

GowlInputRemapVpads *
gowl_inputremap_vpads_new(void)
{
	return g_new0(GowlInputRemapVpads, 1);
}

void
gowl_inputremap_vpads_free(GowlInputRemapVpads *self)
{
	if (self == NULL)
		return;
	g_free(self->error);
	g_free(self);
}

void
gowl_inputremap_vpads_set_loop(
	GowlInputRemapVpads  *self,
	struct wl_event_loop *loop
){
	(void)self;
	(void)loop;
}

gboolean
gowl_inputremap_vpads_render(
	GowlInputRemapVpads        *self,
	GowlInputRemapRule         *rule,
	const GowlInputRemapTarget *target,
	const GowlInputRemapEvent  *event,
	gdouble                     amount
){
	(void)rule;
	(void)target;
	(void)event;
	(void)amount;
	if (self->error == NULL)
		self->error = g_strdup("virtual pads need Linux uinput, which "
		                       "this build does not have");
	return FALSE;
}

void
gowl_inputremap_vpads_sync(
	GowlInputRemapVpads *self,
	GPtrArray           *rules
){
	(void)self;
	(void)rules;
}

void
gowl_inputremap_vpads_destroy_all(GowlInputRemapVpads *self)
{
	(void)self;
}

guint
gowl_inputremap_vpads_count(GowlInputRemapVpads *self)
{
	(void)self;
	return 0;
}

const gchar *
gowl_inputremap_vpads_get_error(GowlInputRemapVpads *self)
{
	return self->error;
}

void
gowl_inputremap_vpads_to_json(
	GowlInputRemapVpads *self,
	JsonBuilder         *b
){
	(void)self;
	json_builder_begin_array(b);
	json_builder_end_array(b);
}

#endif /* GOWL_HAVE_EVDEV_PADS */
