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
 * gowl-input-remap-core.c - The compositor half of per-device remapping.
 *
 * Every keyboard normally joins one wlr_keyboard_group, which is what
 * lets a modifier held on one keyboard apply to a key on another -- and
 * also what makes it impossible to tell which physical device a key came
 * from.  A device a GowlInputRemapper CLAIMS is kept out of the group
 * and heard on its own listeners, so its keys never touch the shared xkb
 * state until the remapper has decided what they become.  Pointers are
 * different: wlr_cursor already hands every button over with its
 * device, so a claimed pointer stays attached (motion is untouched) and
 * only its buttons and wheel notches are offered to the remapper.
 *
 * What a remap produces re-enters the SAME decisions real input takes:
 * a key goes through compositor_handle_key() (keybinds, module binds,
 * the embedder intercept, InputCapture, then the client), a button
 * through compositor_handle_button().  Both as real input, not
 * synthetic: a pedal is hardware, so the recorder records what it
 * produced and a KVM capture carries it to the other machine.  The only
 * difference is key repeat, which is never armed for a remapped key.
 *
 * Invariants:
 *
 *   - No remapper active => every hook returns FALSE before touching
 *     anything, and the compositor behaves exactly as it did before this
 *     file existed.  tests/test-input-remap-fastpath.sh holds that.
 *   - One press, at most one output.  The release replays whatever the
 *     PRESS produced (the `held' table), so a rule added or removed
 *     while a pedal is down cannot strand a key or a button.
 *   - Super+Escape on a claimed keyboard always reaches the escape
 *     hatches (InputCapture break, recorder force-stop), whatever the
 *     rules say.
 *   - While the session is locked, actions, commands and callbacks do
 *     not run; keys and buttons go through the lock-aware pipelines.
 */

#include "gowl-core-private.h"
#include "interfaces/gowl-input-remapper.h"
#include "boxed/gowl-input-remap-rule.h"

#include <wlr/backend/libinput.h>
#include <linux/input-event-codes.h>
#include <string.h>

/* The most modifier keys one key target can hold (one per GowlKeyMod
 * bit it supports). */
#define GOWL_REMAP_MAX_MOD_KEYS (6)

/* A wheel notch in value120 units. */
#define GOWL_REMAP_NOTCH (120)

/*
 * What one press produced, so its release can mirror it exactly.  The
 * rule is referenced, which keeps the target (owned by the rule) alive
 * across a rule being removed mid-press.
 */
typedef struct {
	GowlInputRemapRule       *rule;
	const GowlInputRemapTarget *target;
	GowlInputRemapTargetKind  kind;
	guint32                   out_code;
	guint32                   mod_keys[GOWL_REMAP_MAX_MOD_KEYS];
	guint                     n_mod_keys;
} GowlRemapHeld;

/* One keyboard or pointer the remap core knows about. */
typedef struct {
	GowlCompositor          *compositor;
	struct wlr_input_device *device;
	GowlInputDeviceInfo     *info;
	struct wl_listener       destroy;
	/* claimed keyboards only: heard directly, not through the group */
	struct wl_listener       key;
	gboolean                 listening;
	/* identify mode: an extra observer on every device */
	struct wl_listener       identify_key;
	struct wl_listener       identify_button;
	gboolean                 identifying;
	/* input code -> GowlRemapHeld* for everything currently down */
	GHashTable              *held;
	/* sub-notch wheel travel, per axis, in value120 units */
	gint32                   wheel_acc[2];
} GowlRemapDevice;

/* identify mode state (self->remap_identify) */
typedef struct {
	GowlInputRemapIdentifyFunc func;
	gpointer                   user_data;
} GowlRemapIdentify;

static void remap_device_release(GowlRemapDevice *rd);
static void identify_attach(GowlRemapDevice *rd);
static void identify_detach(GowlRemapDevice *rd);

/* --- small helpers --- */

static GowlInputRemapper *
active_remapper(GowlCompositor *self)
{
	if (self->module_mgr == NULL)
		return NULL;
	return (GowlInputRemapper *)gowl_module_manager_get_input_remapper(
		self->module_mgr);
}

static void
held_free(gpointer data)
{
	GowlRemapHeld *h;

	h = (GowlRemapHeld *)data;
	if (h == NULL)
		return;
	g_clear_pointer(&h->rule, gowl_input_remap_rule_unref);
	g_free(h);
}

static gboolean
is_remappable_type(struct wlr_input_device *dev)
{
	return dev->type == WLR_INPUT_DEVICE_KEYBOARD
	       || dev->type == WLR_INPUT_DEVICE_POINTER;
}

/* The identity a rule matches on: name, and for a libinput device its
 * hardware ids and sysname. */
static GowlInputDeviceInfo *
describe_device(
	GowlCompositor          *self,
	struct wlr_input_device *dev
){
	GowlInputDeviceInfo *info;
	const gchar *sysname;
	guint16 vendor;
	guint16 product;

	sysname = NULL;
	vendor = 0;
	product = 0;
	if (wlr_input_device_is_libinput(dev)) {
		struct libinput_device *li;

		li = wlr_libinput_get_device_handle(dev);
		if (li != NULL) {
			sysname = libinput_device_get_sysname(li);
			vendor = (guint16)libinput_device_get_id_vendor(li);
			product = (guint16)libinput_device_get_id_product(li);
		}
	}
	info = gowl_input_device_info_new(
		++self->remap_next_id,
		dev->type == WLR_INPUT_DEVICE_KEYBOARD
			? GOWL_INPUT_REMAP_DEVICE_KEYBOARD
			: GOWL_INPUT_REMAP_DEVICE_POINTER,
		dev->name, sysname, vendor, product);
	return info;
}

static void
remap_device_free(GowlRemapDevice *rd)
{
	g_clear_pointer(&rd->held, g_hash_table_unref);
	g_clear_pointer(&rd->info, gowl_input_device_info_free);
	g_free(rd);
}

/* The device went away: releases whatever it held down, then forgets
 * it.  wlroots drops a destroyed keyboard from the group by itself. */
static void
on_remap_device_destroy(
	struct wl_listener *listener,
	void               *data
){
	GowlRemapDevice *rd;
	GowlCompositor  *self;

	(void)data;
	rd = wl_container_of(listener, rd, destroy);
	self = rd->compositor;

	if (rd->info->claimed) {
		GowlInputRemapper *r;

		remap_device_release(rd);
		rd->info->claimed = FALSE;
		r = active_remapper(self);
		if (r != NULL)
			gowl_input_remapper_device_changed(r, rd->info, FALSE);
	}
	identify_detach(rd);
	wl_list_remove(&rd->destroy.link);
	g_hash_table_remove(self->remap_devices, rd->device);
	remap_device_free(rd);
}

/* The record for @dev, made on first sight. */
static GowlRemapDevice *
remap_device_get(
	GowlCompositor          *self,
	struct wlr_input_device *dev
){
	GowlRemapDevice *rd;

	if (self->remap_devices == NULL)
		self->remap_devices = g_hash_table_new(g_direct_hash,
		                                       g_direct_equal);
	rd = g_hash_table_lookup(self->remap_devices, dev);
	if (rd != NULL)
		return rd;

	rd = g_new0(GowlRemapDevice, 1);
	rd->compositor = self;
	rd->device = dev;
	rd->info = describe_device(self, dev);
	rd->held = g_hash_table_new_full(g_direct_hash, g_direct_equal,
	                                 NULL, held_free);
	rd->destroy.notify = on_remap_device_destroy;
	wl_signal_add(&dev->events.destroy, &rd->destroy);
	wl_list_init(&rd->key.link);
	wl_list_init(&rd->identify_key.link);
	wl_list_init(&rd->identify_button.link);
	g_hash_table_insert(self->remap_devices, dev, rd);

	/* A device plugged in during identify mode is observed too */
	if (self->remap_identify != NULL)
		identify_attach(rd);
	return rd;
}

/* --- delivering outputs --- */

/*
 * Applies one key edge to the group's xkb state and tells the seat the
 * resulting modifiers.  The same mechanism gowl_compositor_inject_key()
 * uses, for the same reason: one xkb state, updated by keycode, is what
 * keeps a modifier held on two devices correct when either lets go.
 */
static void
group_update_key(
	GowlCompositor *self,
	guint32         keycode,
	gboolean        pressed
){
	struct wlr_keyboard *kb;

	kb = &self->wlr_kb_group->keyboard;
	if (kb->xkb_state != NULL) {
		xkb_state_update_key(kb->xkb_state, keycode + 8,
			pressed ? XKB_KEY_DOWN : XKB_KEY_UP);
		kb->modifiers.depressed = xkb_state_serialize_mods(kb->xkb_state,
			XKB_STATE_MODS_DEPRESSED);
		kb->modifiers.latched = xkb_state_serialize_mods(kb->xkb_state,
			XKB_STATE_MODS_LATCHED);
		kb->modifiers.locked = xkb_state_serialize_mods(kb->xkb_state,
			XKB_STATE_MODS_LOCKED);
		kb->modifiers.group = xkb_state_serialize_layout(kb->xkb_state,
			XKB_STATE_LAYOUT_EFFECTIVE);
	}
	wlr_seat_set_keyboard(self->wlr_seat, kb);
	wlr_seat_keyboard_notify_modifiers(self->wlr_seat, &kb->modifiers);
}

/* One key edge through the whole compositor key decision. */
static void
feed_key(
	GowlCompositor *self,
	guint32         keycode,
	gboolean        pressed,
	guint32         time_msec
){
	if (self->wlr_kb_group == NULL || self->wlr_seat == NULL)
		return;
	group_update_key(self, keycode, pressed);
	gowl_compositor_remap_handle_key(self, keycode,
		pressed ? WL_KEYBOARD_KEY_STATE_PRESSED
		        : WL_KEYBOARD_KEY_STATE_RELEASED,
		time_msec);
}

/* Whether the group keyboard already holds @keycode down. */
static gboolean
group_key_is_down(
	GowlCompositor *self,
	guint32         keycode
){
	struct wlr_keyboard *kb;
	gsize i;

	kb = &self->wlr_kb_group->keyboard;
	for (i = 0; i < kb->num_keycodes; i++)
		if (kb->keycodes[i] == keycode)
			return TRUE;
	return FALSE;
}

/* The first keycode producing @keysym in @keymap, and the shift level
 * it sits at.  Returns an xkb keycode (evdev + 8), 0 when none. */
static xkb_keycode_t
keysym_to_keycode(
	struct xkb_keymap *keymap,
	xkb_keysym_t       keysym,
	xkb_level_index_t *out_level
){
	xkb_keycode_t kc;

	for (kc = xkb_keymap_min_keycode(keymap);
	     kc <= xkb_keymap_max_keycode(keymap); kc++) {
		xkb_layout_index_t layout;
		xkb_layout_index_t n_layouts;

		n_layouts = xkb_keymap_num_layouts_for_key(keymap, kc);
		for (layout = 0; layout < n_layouts; layout++) {
			xkb_level_index_t level;
			xkb_level_index_t n_levels;

			n_levels = xkb_keymap_num_levels_for_key(keymap, kc, layout);
			for (level = 0; level < n_levels; level++) {
				const xkb_keysym_t *syms;
				gint n;
				gint i;

				n = xkb_keymap_key_get_syms_by_level(keymap, kc, layout,
				                                     level, &syms);
				for (i = 0; i < n; i++) {
					if (syms[i] == keysym) {
						*out_level = level;
						return kc;
					}
				}
			}
		}
	}
	return 0;
}

/* The evdev key that produces one GowlKeyMod bit in @keymap. */
static guint32
modifier_keycode(
	struct xkb_keymap *keymap,
	guint32            mod_bit
){
	static const struct {
		guint32      bit;
		xkb_keysym_t sym;
		guint32      fallback;
	} table[] = {
		{ GOWL_KEY_MOD_LOGO,  XKB_KEY_Super_L,          KEY_LEFTMETA  },
		{ GOWL_KEY_MOD_SHIFT, XKB_KEY_Shift_L,          KEY_LEFTSHIFT },
		{ GOWL_KEY_MOD_CTRL,  XKB_KEY_Control_L,        KEY_LEFTCTRL  },
		{ GOWL_KEY_MOD_ALT,   XKB_KEY_Alt_L,            KEY_LEFTALT   },
		{ GOWL_KEY_MOD_MOD3,  XKB_KEY_Hyper_L,          0             },
		{ GOWL_KEY_MOD_MOD5,  XKB_KEY_ISO_Level3_Shift, KEY_RIGHTALT  },
	};
	guint i;

	for (i = 0; i < G_N_ELEMENTS(table); i++) {
		xkb_level_index_t level;
		xkb_keycode_t kc;

		if (table[i].bit != mod_bit)
			continue;
		kc = keymap != NULL ? keysym_to_keycode(keymap, table[i].sym, &level)
		                    : 0;
		return kc != 0 ? (guint32)(kc - 8) : table[i].fallback;
	}
	return 0;
}

/*
 * Presses a key target: resolves a keysym against the live keymap
 * (adding Shift when the symbol sits on a shifted level), holds the
 * target's modifiers in the group state, then feeds the key.  What was
 * held is recorded in @h for the release.
 */
static void
press_key_target(
	GowlCompositor             *self,
	const GowlInputRemapTarget *t,
	guint32                     time_msec,
	GowlRemapHeld              *h
){
	struct xkb_keymap *keymap;
	guint32 keycode;
	guint32 mods;
	guint32 bit;

	keymap = self->wlr_kb_group->keyboard.keymap;
	keycode = gowl_input_remap_target_get_code(t);
	mods = gowl_input_remap_target_get_modifiers(t);

	if (keycode == 0) {
		xkb_level_index_t level;
		xkb_keycode_t kc;

		level = 0;
		kc = keymap != NULL
			? keysym_to_keycode(keymap,
			      (xkb_keysym_t)gowl_input_remap_target_get_keysym(t),
			      &level)
			: 0;
		if (kc == 0) {
			g_debug("input-remap: keysym 0x%x is on no key of the "
			        "current layout; dropped",
			        gowl_input_remap_target_get_keysym(t));
			h->kind = GOWL_INPUT_REMAP_TARGET_DROP;
			return;
		}
		keycode = (guint32)(kc - 8);
		if (level > 0)
			mods |= GOWL_KEY_MOD_SHIFT;
	}

	/* Modifiers first, each only if not already physically held --
	 * pressing a held key twice would leave it stuck on release. */
	h->n_mod_keys = 0;
	for (bit = 1; bit != 0 && bit <= GOWL_KEY_MOD_MOD5; bit <<= 1) {
		guint32 mk;

		if ((mods & bit) == 0)
			continue;
		mk = modifier_keycode(keymap, bit);
		if (mk == 0 || group_key_is_down(self, mk)
		    || h->n_mod_keys >= GOWL_REMAP_MAX_MOD_KEYS)
			continue;
		group_update_key(self, mk, TRUE);
		h->mod_keys[h->n_mod_keys++] = mk;
	}

	h->out_code = keycode;
	feed_key(self, keycode, TRUE, time_msec);
}

/* The release of a key target: the key, then the modifiers it held. */
static void
release_key_target(
	GowlCompositor *self,
	GowlRemapHeld  *h,
	guint32         time_msec
){
	guint i;

	feed_key(self, h->out_code, FALSE, time_msec);
	for (i = h->n_mod_keys; i > 0; i--)
		group_update_key(self, h->mod_keys[i - 1], FALSE);
	h->n_mod_keys = 0;
}

/*
 * Runs the PRESS half of @h (already decided) for @ev, an input of @rd.
 * A keyboard PASS feeds @ev's own key; a pointer PASS is left to the
 * caller, which delivers it the normal way.
 */
static void
apply_press(
	GowlRemapDevice           *rd,
	GowlRemapHeld             *h,
	const GowlInputRemapEvent *ev
){
	GowlCompositor *self;
	gboolean locked;

	self = rd->compositor;
	locked = self->locked;

	switch (h->kind) {
	case GOWL_INPUT_REMAP_TARGET_PASS:
		/* Pointers pass by letting the caller carry on; only a
		 * claimed keyboard's own keys are fed from here. */
		if (ev->kind == GOWL_INPUT_REMAP_EVENT_KEY) {
			h->out_code = ev->code;
			feed_key(self, ev->code, TRUE, ev->time_msec);
		}
		return;
	case GOWL_INPUT_REMAP_TARGET_DROP:
		return;
	case GOWL_INPUT_REMAP_TARGET_KEY:
		press_key_target(self, h->target, ev->time_msec, h);
		return;
	case GOWL_INPUT_REMAP_TARGET_BUTTON:
		h->out_code = gowl_input_remap_target_get_code(h->target);
		gowl_compositor_remap_handle_button(self, h->out_code,
			WL_POINTER_BUTTON_STATE_PRESSED, ev->time_msec);
		return;
	case GOWL_INPUT_REMAP_TARGET_ACTION: {
		GowlKeybindEntry kb;

		if (locked)
			return;
		memset(&kb, 0, sizeof kb);
		kb.action = (gint)gowl_input_remap_target_get_action(h->target);
		kb.arg = (gchar *)gowl_input_remap_target_get_arg(h->target);
		gowl_compositor_run_keybind_entry(self, &kb);
		return;
	}
	case GOWL_INPUT_REMAP_TARGET_COMMAND: {
		g_autofree gchar *reply = NULL;

		if (locked)
			return;
		reply = gowl_compositor_run_command(self,
			gowl_input_remap_target_get_arg(h->target));
		if (reply != NULL && g_str_has_prefix(reply, "ERROR"))
			g_message("input-remap: command '%s': %s",
			          gowl_input_remap_target_get_arg(h->target), reply);
		return;
	}
	case GOWL_INPUT_REMAP_TARGET_CALLBACK:
		if (locked)
			return;
		gowl_input_remap_target_invoke(h->target, h->rule, ev, self);
		return;
	default:
		return;
	}
}

/* The RELEASE half: mirrors exactly what the press did. */
static void
apply_release(
	GowlRemapDevice           *rd,
	GowlRemapHeld             *h,
	const GowlInputRemapEvent *ev
){
	GowlCompositor *self;

	self = rd->compositor;
	switch (h->kind) {
	case GOWL_INPUT_REMAP_TARGET_PASS:
		if (ev->kind == GOWL_INPUT_REMAP_EVENT_KEY && h->out_code != 0)
			feed_key(self, h->out_code, FALSE, ev->time_msec);
		return;
	case GOWL_INPUT_REMAP_TARGET_KEY:
		if (h->out_code != 0)
			release_key_target(self, h, ev->time_msec);
		return;
	case GOWL_INPUT_REMAP_TARGET_BUTTON:
		gowl_compositor_remap_handle_button(self, h->out_code,
			WL_POINTER_BUTTON_STATE_RELEASED, ev->time_msec);
		return;
	case GOWL_INPUT_REMAP_TARGET_CALLBACK:
		if (!self->locked)
			gowl_input_remap_target_invoke(h->target, h->rule, ev, self);
		return;
	case GOWL_INPUT_REMAP_TARGET_DROP:
	case GOWL_INPUT_REMAP_TARGET_ACTION:
	case GOWL_INPUT_REMAP_TARGET_COMMAND:
	default:
		/* An action or command ran on the press; the release is
		 * swallowed so it cannot leak through to anything. */
		return;
	}
}

/*
 * Decides what one PRESS turns into: the remapper's target, or -- for a
 * rule that does not mention the input -- a drop or a pass as the rule
 * says.  No rule at all is a pass.
 */
static GowlRemapHeld *
decide(
	GowlRemapDevice           *rd,
	const GowlInputRemapEvent *ev
){
	GowlRemapHeld *h;
	GowlInputRemapper *r;
	const GowlInputRemapTarget *target;

	h = g_new0(GowlRemapHeld, 1);
	r = active_remapper(rd->compositor);
	target = NULL;
	h->rule = r != NULL ? gowl_input_remapper_map_event(r, rd->info, ev,
	                                                    &target)
	                    : NULL;
	if (h->rule == NULL)
		h->kind = GOWL_INPUT_REMAP_TARGET_PASS;
	else if (target == NULL)
		h->kind = gowl_input_remap_rule_get_drop_unmatched(h->rule)
		          ? GOWL_INPUT_REMAP_TARGET_DROP
		          : GOWL_INPUT_REMAP_TARGET_PASS;
	else {
		h->target = target;
		h->kind = gowl_input_remap_target_get_kind(target);
	}
	return h;
}

/*
 * One edge of one input on a claimed device.  Returns TRUE when the
 * remap core consumed it; FALSE only for a pointer input that passes
 * through, which the caller then delivers the normal way.
 */
static gboolean
handle_edge(
	GowlRemapDevice           *rd,
	const GowlInputRemapEvent *ev,
	gboolean                   escape
){
	GowlCompositor *self;
	GowlRemapHeld *h;
	gpointer key;

	self = rd->compositor;
	key = GUINT_TO_POINTER(ev->code);

	if (self->idle_mgr != NULL)
		gowl_idle_manager_note_activity(self->idle_mgr);

	if (!ev->pressed) {
		gboolean consumed;

		/* The release mirrors its press.  A release with no press on
		 * record belongs to a press from before the claim: a pointer
		 * delivers it the normal way, a keyboard's was already
		 * released by the group when the device left it. */
		h = g_hash_table_lookup(rd->held, key);
		if (h == NULL)
			return ev->kind == GOWL_INPUT_REMAP_EVENT_KEY;
		consumed = !(h->kind == GOWL_INPUT_REMAP_TARGET_PASS
		             && ev->kind != GOWL_INPUT_REMAP_EVENT_KEY);
		apply_release(rd, h, ev);
		g_hash_table_remove(rd->held, key);
		return consumed;
	}

	/* A press of an input that is somehow already down (a device that
	 * repeats in hardware): the first press owns it until released. */
	h = g_hash_table_lookup(rd->held, key);
	if (h != NULL)
		return !(h->kind == GOWL_INPUT_REMAP_TARGET_PASS
		         && ev->kind != GOWL_INPUT_REMAP_EVENT_KEY);

	if (escape) {
		struct wlr_keyboard *kb;
		guint32 wmods;
		guint32 want[2];
		guint i;

		/* The escape hatch beats every rule.  It is delivered as
		 * Super[+Shift]+Escape into the group state, so gowl_key_route()
		 * and the recorder hatch see the combination the person actually
		 * pressed on this device, whatever its Super key is mapped to. */
		h = g_new0(GowlRemapHeld, 1);
		h->kind = GOWL_INPUT_REMAP_TARGET_KEY;
		kb = wlr_keyboard_from_input_device(rd->device);
		wmods = wlr_keyboard_get_modifiers(kb);
		want[0] = (wmods & WLR_MODIFIER_LOGO) ? GOWL_KEY_MOD_LOGO : 0;
		want[1] = (wmods & WLR_MODIFIER_SHIFT) ? GOWL_KEY_MOD_SHIFT : 0;
		for (i = 0; i < G_N_ELEMENTS(want); i++) {
			guint32 mk;

			if (want[i] == 0)
				continue;
			mk = modifier_keycode(self->wlr_kb_group->keyboard.keymap,
			                      want[i]);
			if (mk != 0 && !group_key_is_down(self, mk)) {
				group_update_key(self, mk, TRUE);
				h->mod_keys[h->n_mod_keys++] = mk;
			}
		}
		h->out_code = KEY_ESC;
		g_hash_table_insert(rd->held, key, h);
		feed_key(self, KEY_ESC, TRUE, ev->time_msec);
		return TRUE;
	}

	h = decide(rd, ev);
	g_hash_table_insert(rd->held, key, h);
	apply_press(rd, h, ev);
	return !(h->kind == GOWL_INPUT_REMAP_TARGET_PASS
	         && ev->kind != GOWL_INPUT_REMAP_EVENT_KEY);
}

/* A key on a claimed keyboard, heard on the device's own signal. */
static void
on_remap_key(
	struct wl_listener *listener,
	void               *data
){
	GowlRemapDevice *rd;
	struct wlr_keyboard_key_event *event;
	struct wlr_keyboard *kb;
	GowlInputRemapEvent ev;
	gboolean escape;

	rd = wl_container_of(listener, rd, key);
	event = (struct wlr_keyboard_key_event *)data;
	kb = wlr_keyboard_from_input_device(rd->device);

	memset(&ev, 0, sizeof ev);
	ev.kind = GOWL_INPUT_REMAP_EVENT_KEY;
	ev.code = event->keycode;
	ev.pressed = event->state == WL_KEYBOARD_KEY_STATE_PRESSED;
	ev.time_msec = event->time_msec;
	ev.device_id = rd->info->id;

	/* The device's own xkb state is still the one from BEFORE this key
	 * (wlroots updates it after emitting), so a held Super shows. */
	escape = ev.pressed && event->keycode == KEY_ESC
	         && (wlr_keyboard_get_modifiers(kb) & WLR_MODIFIER_LOGO) != 0;

	handle_edge(rd, &ev, escape);
}

/* --- claiming --- */

/* Every held output released, in no particular order: what taking a
 * device away from a remapper (or losing it) must do. */
static void
remap_device_release(GowlRemapDevice *rd)
{
	GHashTableIter iter;
	gpointer key;
	gpointer value;

	g_hash_table_iter_init(&iter, rd->held);
	while (g_hash_table_iter_next(&iter, &key, &value)) {
		GowlRemapHeld *h;
		GowlInputRemapEvent ev;

		h = (GowlRemapHeld *)value;
		memset(&ev, 0, sizeof ev);
		ev.kind = rd->device->type == WLR_INPUT_DEVICE_KEYBOARD
		          ? GOWL_INPUT_REMAP_EVENT_KEY
		          : GOWL_INPUT_REMAP_EVENT_BUTTON;
		ev.code = GPOINTER_TO_UINT(key);
		ev.pressed = FALSE;
		ev.device_id = rd->info->id;
		apply_release(rd, h, &ev);
		g_hash_table_iter_remove(&iter);
	}
	rd->wheel_acc[0] = 0;
	rd->wheel_acc[1] = 0;
	if (rd->listening) {
		wl_list_remove(&rd->key.link);
		wl_list_init(&rd->key.link);
		rd->listening = FALSE;
	}
}

/* Claims @rd: a keyboard gets its own listener (the caller keeps it out
 * of the group); a pointer is only marked. */
static void
remap_device_claim(GowlRemapDevice *rd)
{
	if (rd->device->type == WLR_INPUT_DEVICE_KEYBOARD && !rd->listening) {
		struct wlr_keyboard *kb;

		kb = wlr_keyboard_from_input_device(rd->device);
		rd->key.notify = on_remap_key;
		wl_signal_add(&kb->events.key, &rd->key);
		rd->listening = TRUE;
	}
	rd->info->claimed = TRUE;
}

/**
 * gowl_input_remap_core_try_claim: (skip)
 * @self: the compositor
 * @dev: a keyboard or pointer that just appeared
 *
 * Asks the active remapper whether it claims @dev.  For a keyboard the
 * caller must then keep it OUT of the keyboard group.  Returns FALSE at
 * once when no remapper is active.
 *
 * Returns: %TRUE when @dev is claimed
 */
gboolean
gowl_input_remap_core_try_claim(
	GowlCompositor          *self,
	struct wlr_input_device *dev
){
	GowlInputRemapper *r;
	GowlRemapDevice *rd;

	r = active_remapper(self);
	if (r == NULL || dev == NULL || !is_remappable_type(dev))
		return FALSE;

	rd = remap_device_get(self, dev);
	if (!gowl_input_remapper_claims_device(r, rd->info))
		return FALSE;
	remap_device_claim(rd);
	gowl_input_remapper_device_changed(r, rd->info, TRUE);
	return TRUE;
}

/* One device, re-decided against the current rules. */
static void
reevaluate_device(
	struct wlr_input_device *dev,
	gpointer                 user_data
){
	GowlCompositor *self;
	GowlInputRemapper *r;
	GowlRemapDevice *rd;
	gboolean want;

	self = GOWL_COMPOSITOR(user_data);
	if (!is_remappable_type(dev))
		return;

	r = active_remapper(self);
	rd = self->remap_devices != NULL
		? g_hash_table_lookup(self->remap_devices, dev) : NULL;
	if (rd == NULL && r == NULL)
		return;         /* never seen, nothing to claim it: untouched */
	if (rd == NULL)
		rd = remap_device_get(self, dev);

	want = r != NULL && gowl_input_remapper_claims_device(r, rd->info);
	if (want == rd->info->claimed)
		return;

	if (want) {
		/* Out of the group first: wlroots releases, on the group, any
		 * key this device still holds, so nothing is stuck down. */
		if (dev->type == WLR_INPUT_DEVICE_KEYBOARD)
			wlr_keyboard_group_remove_keyboard(self->wlr_kb_group,
				wlr_keyboard_from_input_device(dev));
		remap_device_claim(rd);
	} else {
		remap_device_release(rd);
		rd->info->claimed = FALSE;
		if (dev->type == WLR_INPUT_DEVICE_KEYBOARD
		    && !wlr_keyboard_group_add_keyboard(self->wlr_kb_group,
		           wlr_keyboard_from_input_device(dev)))
			g_warning("input-remap: '%s' could not rejoin the keyboard "
			          "group (keymap mismatch)", dev->name);
	}
	if (r != NULL)
		gowl_input_remapper_device_changed(r, rd->info, want);
}

/**
 * gowl_compositor_input_remap_reevaluate:
 * @self: a #GowlCompositor
 *
 * Re-decides, against the active #GowlInputRemapper's current rules,
 * which connected devices are claimed: a newly claimed keyboard leaves
 * the shared keyboard group, a released one rejoins it, and anything a
 * released device held down is released first.  A remapper calls this
 * after adding or removing rules, and when it is enabled or disabled.
 * With no remapper active every claim is released.
 */
void
gowl_compositor_input_remap_reevaluate(GowlCompositor *self)
{
	guint32 caps;

	g_return_if_fail(GOWL_IS_COMPOSITOR(self));

	if (self->wlr_kb_group == NULL)
		return;
	/* Nothing active and nothing ever claimed: nothing to re-decide,
	 * and the seat is left exactly as it is. */
	if (active_remapper(self) == NULL && self->remap_devices == NULL)
		return;
	gowl_input_config_foreach_device(self, reevaluate_device, self);

	/* A pedal may be the only keyboard left outside the group */
	if (self->wlr_seat != NULL) {
		caps = WL_SEAT_CAPABILITY_POINTER;
		if (!wl_list_empty(&self->wlr_kb_group->devices)
		    || gowl_input_remap_core_has_claimed_keyboard(self))
			caps |= WL_SEAT_CAPABILITY_KEYBOARD;
		wlr_seat_set_capabilities(self->wlr_seat, caps);
	}
}

/**
 * gowl_input_remap_core_has_claimed_keyboard: (skip)
 * @self: the compositor
 *
 * Returns: %TRUE when a claimed keyboard exists, so the seat keeps its
 *   keyboard capability with only a pedal plugged in
 */
gboolean
gowl_input_remap_core_has_claimed_keyboard(GowlCompositor *self)
{
	GHashTableIter iter;
	gpointer value;

	if (self->remap_devices == NULL)
		return FALSE;
	g_hash_table_iter_init(&iter, self->remap_devices);
	while (g_hash_table_iter_next(&iter, NULL, &value)) {
		GowlRemapDevice *rd = (GowlRemapDevice *)value;

		if (rd->info->claimed
		    && rd->device->type == WLR_INPUT_DEVICE_KEYBOARD)
			return TRUE;
	}
	return FALSE;
}

/* --- pointer hooks --- */

/* The claimed record for a pointer, or NULL: the fast path. */
static GowlRemapDevice *
claimed_pointer(
	GowlCompositor          *self,
	struct wlr_input_device *dev
){
	GowlRemapDevice *rd;

	if (self->remap_devices == NULL || dev == NULL)
		return NULL;
	rd = g_hash_table_lookup(self->remap_devices, dev);
	if (rd == NULL || !rd->info->claimed)
		return NULL;
	return rd;
}

/**
 * gowl_input_remap_core_button: (skip)
 * @self: the compositor
 * @dev: the pointer the button came from
 * @button: BTN_* code
 * @state: a wl_pointer_button_state
 * @time_msec: event time
 *
 * Returns: %TRUE when the remap core consumed the button; %FALSE (at
 *   once, when no device is claimed) to deliver it the normal way
 */
gboolean
gowl_input_remap_core_button(
	GowlCompositor          *self,
	struct wlr_input_device *dev,
	guint32                  button,
	guint32                  state,
	guint32                  time_msec
){
	GowlRemapDevice *rd;
	GowlInputRemapEvent ev;

	rd = claimed_pointer(self, dev);
	if (rd == NULL)
		return FALSE;

	memset(&ev, 0, sizeof ev);
	ev.kind = GOWL_INPUT_REMAP_EVENT_BUTTON;
	ev.code = button;
	ev.pressed = state == WL_POINTER_BUTTON_STATE_PRESSED;
	ev.time_msec = time_msec;
	ev.device_id = rd->info->id;
	return handle_edge(rd, &ev, FALSE);
}

/**
 * gowl_input_remap_core_axis: (skip)
 * @self: the compositor
 * @event: a wheel event from wlr_cursor
 *
 * Wheel travel on a claimed pointer, counted in whole notches (120 per
 * notch, so a high-resolution wheel's quarter-notches add up to one
 * input rather than four).  Each notch is one press and its release.
 * Continuous scrolling (a touchpad) is never remapped.
 *
 * Returns: %TRUE when the remap core consumed the scroll
 */
gboolean
gowl_input_remap_core_axis(
	GowlCompositor                *self,
	struct wlr_pointer_axis_event *event
){
	GowlRemapDevice *rd;
	GowlInputRemapper *r;
	GowlInputRemapEvent ev;
	const GowlInputRemapTarget *probe;
	g_autoptr(GowlInputRemapRule) rule = NULL;
	guint axis;
	gint32 *acc;
	gboolean consumed;

	rd = claimed_pointer(self, event != NULL ? &event->pointer->base : NULL);
	if (rd == NULL || event->source != WL_POINTER_AXIS_SOURCE_WHEEL
	    || event->delta_discrete == 0)
		return FALSE;

	axis = event->orientation == WL_POINTER_AXIS_HORIZONTAL_SCROLL ? 1 : 0;
	memset(&ev, 0, sizeof ev);
	ev.kind = GOWL_INPUT_REMAP_EVENT_AXIS;
	ev.time_msec = event->time_msec;
	ev.device_id = rd->info->id;
	if (axis == 1)
		ev.code = event->delta_discrete < 0 ? GOWL_BUTTON_WHEEL_LEFT
		                                    : GOWL_BUTTON_WHEEL_RIGHT;
	else
		ev.code = event->delta_discrete < 0 ? GOWL_BUTTON_WHEEL_UP
		                                    : GOWL_BUTTON_WHEEL_DOWN;

	/* Does any rule want this direction at all?  If not, the scroll
	 * goes to the client untouched (or is dropped, per the rule). */
	r = active_remapper(self);
	probe = NULL;
	ev.pressed = TRUE;
	rule = r != NULL ? gowl_input_remapper_map_event(r, rd->info, &ev, &probe)
	                 : NULL;
	if (rule == NULL)
		return FALSE;
	if (probe == NULL)
		return gowl_input_remap_rule_get_drop_unmatched(rule);
	if (gowl_input_remap_target_get_kind(probe) == GOWL_INPUT_REMAP_TARGET_PASS)
		return FALSE;

	/* Whole notches only; a reversal starts the count again */
	acc = &rd->wheel_acc[axis];
	if ((*acc > 0 && event->delta_discrete < 0)
	    || (*acc < 0 && event->delta_discrete > 0))
		*acc = 0;
	*acc += event->delta_discrete;

	consumed = TRUE;
	while (*acc >= GOWL_REMAP_NOTCH || *acc <= -GOWL_REMAP_NOTCH) {
		*acc += *acc > 0 ? -GOWL_REMAP_NOTCH : GOWL_REMAP_NOTCH;
		ev.pressed = TRUE;
		handle_edge(rd, &ev, FALSE);
		ev.pressed = FALSE;
		handle_edge(rd, &ev, FALSE);
	}
	return consumed;
}

/* --- device listing --- */

typedef struct {
	GowlCompositor *compositor;
	GPtrArray      *out;
} ListCtx;

static void
list_device(
	struct wlr_input_device *dev,
	gpointer                 user_data
){
	ListCtx *ctx;
	GowlRemapDevice *rd;

	ctx = (ListCtx *)user_data;
	if (!is_remappable_type(dev))
		return;
	rd = remap_device_get(ctx->compositor, dev);
	g_ptr_array_add(ctx->out, gowl_input_device_info_copy(rd->info));
}

/**
 * gowl_compositor_list_input_devices:
 * @self: a #GowlCompositor
 *
 * Every connected keyboard and pointer, with the identity a remap rule
 * matches on and whether it is claimed.  What `inputremap-devices'
 * prints, for finding a pedal's vendor:product.
 *
 * Returns: (transfer full) (element-type GowlInputDeviceInfo): the
 *   devices
 */
GPtrArray *
gowl_compositor_list_input_devices(GowlCompositor *self)
{
	ListCtx ctx;

	g_return_val_if_fail(GOWL_IS_COMPOSITOR(self), NULL);

	ctx.compositor = self;
	ctx.out = g_ptr_array_new_with_free_func(
		(GDestroyNotify)gowl_input_device_info_free);
	gowl_input_config_foreach_device(self, list_device, &ctx);
	return ctx.out;
}

/* --- identify mode --- */

static void
identify_emit(
	GowlRemapDevice           *rd,
	const GowlInputRemapEvent *ev
){
	GowlRemapIdentify *id;

	id = (GowlRemapIdentify *)rd->compositor->remap_identify;
	if (id != NULL && id->func != NULL && ev->pressed)
		id->func(rd->compositor, rd->info, ev, id->user_data);
}

static void
on_identify_key(
	struct wl_listener *listener,
	void               *data
){
	GowlRemapDevice *rd;
	struct wlr_keyboard_key_event *event;
	GowlInputRemapEvent ev;

	rd = wl_container_of(listener, rd, identify_key);
	event = (struct wlr_keyboard_key_event *)data;
	memset(&ev, 0, sizeof ev);
	ev.kind = GOWL_INPUT_REMAP_EVENT_KEY;
	ev.code = event->keycode;
	ev.pressed = event->state == WL_KEYBOARD_KEY_STATE_PRESSED;
	ev.time_msec = event->time_msec;
	ev.device_id = rd->info->id;
	identify_emit(rd, &ev);
}

static void
on_identify_button(
	struct wl_listener *listener,
	void               *data
){
	GowlRemapDevice *rd;
	struct wlr_pointer_button_event *event;
	GowlInputRemapEvent ev;

	rd = wl_container_of(listener, rd, identify_button);
	event = (struct wlr_pointer_button_event *)data;
	memset(&ev, 0, sizeof ev);
	ev.kind = GOWL_INPUT_REMAP_EVENT_BUTTON;
	ev.code = event->button;
	ev.pressed = event->state == WL_POINTER_BUTTON_STATE_PRESSED;
	ev.time_msec = event->time_msec;
	ev.device_id = rd->info->id;
	identify_emit(rd, &ev);
}

/* An observer only: the input still goes wherever it was going. */
static void
identify_attach(GowlRemapDevice *rd)
{
	if (rd->identifying)
		return;
	if (rd->device->type == WLR_INPUT_DEVICE_KEYBOARD) {
		rd->identify_key.notify = on_identify_key;
		wl_signal_add(&wlr_keyboard_from_input_device(rd->device)->events.key,
		              &rd->identify_key);
	} else {
		rd->identify_button.notify = on_identify_button;
		wl_signal_add(&wlr_pointer_from_input_device(rd->device)->events.button,
		              &rd->identify_button);
	}
	rd->identifying = TRUE;
}

static void
identify_detach(GowlRemapDevice *rd)
{
	if (!rd->identifying)
		return;
	wl_list_remove(&rd->identify_key.link);
	wl_list_init(&rd->identify_key.link);
	wl_list_remove(&rd->identify_button.link);
	wl_list_init(&rd->identify_button.link);
	rd->identifying = FALSE;
}

static void
identify_attach_device(
	struct wlr_input_device *dev,
	gpointer                 user_data
){
	if (is_remappable_type(dev))
		identify_attach(remap_device_get(GOWL_COMPOSITOR(user_data), dev));
}

/**
 * gowl_compositor_input_remap_identify_start:
 * @self: a #GowlCompositor
 * @func: (scope forever): called with the device and input for every
 *   key or button press on any keyboard or pointer
 * @user_data: (closure): passed to @func
 *
 * Observes every keyboard and pointer, claimed or not, until
 * gowl_compositor_input_remap_identify_stop().  Nothing is consumed:
 * the input still goes where it was going.  This is how a pedal is
 * found -- press it, and its name and vendor:product arrive.  Starting
 * again replaces the previous observer.
 *
 * Returns: %TRUE when observation started
 */
gboolean
gowl_compositor_input_remap_identify_start(
	GowlCompositor             *self,
	GowlInputRemapIdentifyFunc  func,
	gpointer                    user_data
){
	GowlRemapIdentify *id;

	g_return_val_if_fail(GOWL_IS_COMPOSITOR(self), FALSE);
	g_return_val_if_fail(func != NULL, FALSE);

	id = (GowlRemapIdentify *)self->remap_identify;
	if (id == NULL) {
		id = g_new0(GowlRemapIdentify, 1);
		self->remap_identify = id;
	}
	id->func = func;
	id->user_data = user_data;
	gowl_input_config_foreach_device(self, identify_attach_device, self);
	return TRUE;
}

/**
 * gowl_compositor_input_remap_identify_stop:
 * @self: a #GowlCompositor
 *
 * Stops observing.  Safe to call when not observing.
 */
void
gowl_compositor_input_remap_identify_stop(GowlCompositor *self)
{
	GHashTableIter iter;
	gpointer value;

	g_return_if_fail(GOWL_IS_COMPOSITOR(self));

	if (self->remap_devices != NULL) {
		g_hash_table_iter_init(&iter, self->remap_devices);
		while (g_hash_table_iter_next(&iter, NULL, &value))
			identify_detach((GowlRemapDevice *)value);
	}
	g_clear_pointer(&self->remap_identify, g_free);
}

/**
 * gowl_input_remap_core_finish: (skip)
 * @self: the compositor, during teardown
 *
 * Drops every record and listener before the backend destroys the
 * devices (the per-device destroy listeners would otherwise fire into a
 * compositor that is being finalized).
 */
void
gowl_input_remap_core_finish(GowlCompositor *self)
{
	GHashTableIter iter;
	gpointer value;

	g_clear_pointer(&self->remap_identify, g_free);
	if (self->remap_devices == NULL)
		return;
	g_hash_table_iter_init(&iter, self->remap_devices);
	while (g_hash_table_iter_next(&iter, NULL, &value)) {
		GowlRemapDevice *rd = (GowlRemapDevice *)value;

		if (rd->listening)
			wl_list_remove(&rd->key.link);
		identify_detach(rd);
		wl_list_remove(&rd->destroy.link);
		g_hash_table_iter_remove(&iter);
		remap_device_free(rd);
	}
	g_clear_pointer(&self->remap_devices, g_hash_table_unref);
}
