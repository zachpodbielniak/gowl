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
 * gowl-input-remap-rule.c - Per-device input remap rules.
 *
 * The one-to-one guarantee lives here, in the shape of the data, rather
 * than in documentation: every input maps to exactly one target, the
 * mapping functions replace instead of append, and the YAML loader
 * refuses anything that looks like a sequence, a macro, a delay or a
 * repeat with GOWL_INPUT_REMAP_ERROR_NOT_ONE_TO_ONE.  There is simply
 * nowhere in a rule to put a second output.
 */

#include "gowl-input-remap-rule.h"
#include "gowl-input-remap-rule-private.h"

#include <stdio.h>
#include <string.h>
#include <linux/input-event-codes.h>
#include <xkbcommon/xkbcommon.h>

/* --- evdev names --- */

typedef struct {
	const gchar *name;
	guint32      code;
} GowlEvdevName;

/*
 * Every KEY_* and BTN_* the kernel headers define, generated at build
 * time from <linux/input-event-codes.h> (see the Makefile rule for
 * gowl-evdev-names.inc) so the table is always the running kernel's --
 * a hand-written list would be wrong the day a new key code lands.
 */
static const GowlEvdevName evdev_names[] = {
#include "gowl-evdev-names.inc"
	{ NULL, 0 }
};

/* Friendly pointer-button names, accepted on both sides of a rule. */
static const GowlEvdevName button_names[] = {
	{ "left",    BTN_LEFT    },
	{ "right",   BTN_RIGHT   },
	{ "middle",  BTN_MIDDLE  },
	{ "side",    BTN_SIDE    },
	{ "extra",   BTN_EXTRA   },
	{ "forward", BTN_FORWARD },
	{ "back",    BTN_BACK    },
	{ "task",    BTN_TASK    },
	{ "button1", BTN_LEFT    },
	{ "button2", BTN_MIDDLE  },
	{ "button3", BTN_RIGHT   },
	{ "button8", BTN_SIDE    },
	{ "button9", BTN_EXTRA   },
	{ NULL, 0 }
};

/* Wheel notches, as rule inputs. */
static const GowlEvdevName wheel_names[] = {
	{ "WHEEL_UP",    GOWL_BUTTON_WHEEL_UP    },
	{ "WHEEL_DOWN",  GOWL_BUTTON_WHEEL_DOWN  },
	{ "WHEEL_LEFT",  GOWL_BUTTON_WHEEL_LEFT  },
	{ "WHEEL_RIGHT", GOWL_BUTTON_WHEEL_RIGHT },
	{ NULL, 0 }
};

/* Modifier names for a key target.  The keybind parser has the same
 * table but warns on a miss; a rule wants a GError instead. */
static const GowlEvdevName modifier_names[] = {
	{ "super",   GOWL_KEY_MOD_LOGO  },
	{ "logo",    GOWL_KEY_MOD_LOGO  },
	{ "mod4",    GOWL_KEY_MOD_LOGO  },
	{ "shift",   GOWL_KEY_MOD_SHIFT },
	{ "ctrl",    GOWL_KEY_MOD_CTRL  },
	{ "control", GOWL_KEY_MOD_CTRL  },
	{ "alt",     GOWL_KEY_MOD_ALT   },
	{ "mod1",    GOWL_KEY_MOD_ALT   },
	{ "mod3",    GOWL_KEY_MOD_MOD3  },
	{ "mod5",    GOWL_KEY_MOD_MOD5  },
	{ NULL, 0 }
};

#define GOWL_INPUT_REMAP_MOD_MASK \
	((guint32)(GOWL_KEY_MOD_LOGO | GOWL_KEY_MOD_SHIFT | GOWL_KEY_MOD_CTRL \
	           | GOWL_KEY_MOD_ALT | GOWL_KEY_MOD_MOD3 | GOWL_KEY_MOD_MOD5))

/* Keys that would make a rule anything other than one-to-one.  Named
 * rather than merely unknown so the error says WHY it was refused. */
static const gchar *const forbidden_keys[] = {
	"sequence", "sequences", "macro", "macros", "delay", "delay-ms",
	"repeat", "repeat-rate", "keys", "then", "chain", "times", "tap",
	"hold", "timeout", "interval", "script", NULL
};

static guint callback_generation = 0;

G_DEFINE_QUARK(gowl-input-remap-error-quark, gowl_input_remap_error)

/* --- Internal types --- */

struct _GowlInputRemapTarget {
	GowlInputRemapTargetKind kind;
	guint32                  code;
	guint32                  keysym;
	guint32                  modifiers;
	GowlAction               action;
	gchar                   *arg;
	GowlInputRemapCallback   callback;
	gpointer                 user_data;
	GDestroyNotify           destroy;
};

struct _GowlInputRemapRule {
	gint                      ref_count;
	gchar                    *name;
	gchar                    *match_name;
	gchar                    *match_sysname;
	guint16                   vendor;
	guint16                   product;
	GowlInputRemapDeviceType  match_type;
	gboolean                  drop_unmatched;
	gboolean                  log;
	/* GUINT_TO_POINTER(input code) -> GowlInputRemapTarget* (owned) */
	GHashTable               *map;
};

static void
target_free(gpointer data)
{
	GowlInputRemapTarget *t;

	t = (GowlInputRemapTarget *)data;
	if (t == NULL)
		return;
	if (t->destroy != NULL && t->user_data != NULL)
		t->destroy(t->user_data);
	g_free(t->arg);
	g_free(t);
}

/* A valid rule input: a real evdev key/button code or a wheel notch.
 * 0 is KEY_RESERVED, never emitted by hardware. */
static gboolean
input_is_valid(guint32 input)
{
	return (input > 0 && input <= KEY_MAX)
	       || (input >= GOWL_BUTTON_WHEEL_UP
	           && input <= GOWL_BUTTON_WHEEL_RIGHT);
}

static gboolean
check_input(
	guint32   input,
	GError  **error
){
	if (input_is_valid(input))
		return TRUE;
	g_set_error(error, GOWL_INPUT_REMAP_ERROR,
	            GOWL_INPUT_REMAP_ERROR_UNKNOWN_CODE,
	            "input code 0x%x is neither a key, a button nor a wheel "
	            "notch", input);
	return FALSE;
}

/* Replaces whatever @input mapped to.  This is the one place a mapping
 * is stored, and it is a replace, never an append. */
static void
store_target(
	GowlInputRemapRule   *self,
	guint32               input,
	GowlInputRemapTarget *target
){
	g_hash_table_replace(self->map, GUINT_TO_POINTER(input), target);
}

/* --- GowlInputRemapEvent --- */

G_DEFINE_BOXED_TYPE(GowlInputRemapEvent, gowl_input_remap_event,
                    gowl_input_remap_event_copy,
                    gowl_input_remap_event_free)

/**
 * gowl_input_remap_event_copy:
 * @self: a #GowlInputRemapEvent
 *
 * Returns: (transfer full): a copy of @self
 */
GowlInputRemapEvent *
gowl_input_remap_event_copy(const GowlInputRemapEvent *self)
{
	g_return_val_if_fail(self != NULL, NULL);
	return (GowlInputRemapEvent *)g_memdup2(self, sizeof *self);
}

/**
 * gowl_input_remap_event_free:
 * @self: (nullable): a #GowlInputRemapEvent
 *
 * Frees @self.
 */
void
gowl_input_remap_event_free(GowlInputRemapEvent *self)
{
	g_free(self);
}

/* --- GowlInputDeviceInfo --- */

G_DEFINE_BOXED_TYPE(GowlInputDeviceInfo, gowl_input_device_info,
                    gowl_input_device_info_copy,
                    gowl_input_device_info_free)

/**
 * gowl_input_device_info_new:
 * @id: the compositor-assigned device id
 * @type: keyboard or pointer
 * @name: (nullable): the libinput device name
 * @sysname: (nullable): the kernel sysname
 * @vendor: the vendor id, 0 when unknown
 * @product: the product id, 0 when unknown
 *
 * Returns: (transfer full): a new #GowlInputDeviceInfo, not claimed
 */
GowlInputDeviceInfo *
gowl_input_device_info_new(
	guint                     id,
	GowlInputRemapDeviceType  type,
	const gchar              *name,
	const gchar              *sysname,
	guint16                   vendor,
	guint16                   product
){
	GowlInputDeviceInfo *info;

	info = g_new0(GowlInputDeviceInfo, 1);
	info->id = id;
	info->type = type;
	info->name = g_strdup(name);
	info->sysname = g_strdup(sysname);
	info->vendor = vendor;
	info->product = product;
	info->claimed = FALSE;
	return info;
}

/**
 * gowl_input_device_info_copy:
 * @self: a #GowlInputDeviceInfo
 *
 * Returns: (transfer full): a deep copy of @self
 */
GowlInputDeviceInfo *
gowl_input_device_info_copy(const GowlInputDeviceInfo *self)
{
	GowlInputDeviceInfo *copy;

	g_return_val_if_fail(self != NULL, NULL);

	copy = gowl_input_device_info_new(self->id, self->type, self->name,
	                                  self->sysname, self->vendor,
	                                  self->product);
	copy->claimed = self->claimed;
	return copy;
}

/**
 * gowl_input_device_info_free:
 * @self: (nullable): a #GowlInputDeviceInfo
 *
 * Frees @self and its strings.
 */
void
gowl_input_device_info_free(GowlInputDeviceInfo *self)
{
	if (self == NULL)
		return;
	g_free(self->name);
	g_free(self->sysname);
	g_free(self);
}

/**
 * gowl_input_device_info_to_string:
 * @self: a #GowlInputDeviceInfo
 *
 * One line describing the device, in the form the `inputremap-devices'
 * command prints: `ID TYPE VENDOR:PRODUCT SYSNAME CLAIMED "NAME"'.
 *
 * Returns: (transfer full): the description
 */
gchar *
gowl_input_device_info_to_string(const GowlInputDeviceInfo *self)
{
	GEnumClass *klass;
	GEnumValue *val;
	const gchar *type_nick;
	gchar *out;

	g_return_val_if_fail(self != NULL, NULL);

	klass = (GEnumClass *)g_type_class_ref(GOWL_TYPE_INPUT_REMAP_DEVICE_TYPE);
	val = g_enum_get_value(klass, (gint)self->type);
	type_nick = val != NULL ? val->value_nick : "any";
	out = g_strdup_printf("%u %s %04x:%04x %s %s \"%s\"",
	                      self->id, type_nick,
	                      (guint)self->vendor, (guint)self->product,
	                      self->sysname != NULL ? self->sysname : "-",
	                      self->claimed ? "claimed" : "unclaimed",
	                      self->name != NULL ? self->name : "");
	g_type_class_unref(klass);
	return out;
}

/* --- GowlInputRemapRule: lifecycle --- */

G_DEFINE_BOXED_TYPE(GowlInputRemapRule, gowl_input_remap_rule,
                    gowl_input_remap_rule_ref,
                    gowl_input_remap_rule_unref)

/**
 * gowl_input_remap_rule_new:
 * @name: the rule's name; also its id for removal at runtime
 *
 * Creates an empty rule.  It matches no device until at least one
 * match criterion is set (see gowl_input_remap_rule_has_match()), so a
 * half-built rule can never claim every keyboard on the machine.
 *
 * Returns: (transfer full): a new #GowlInputRemapRule
 */
GowlInputRemapRule *
gowl_input_remap_rule_new(const gchar *name)
{
	GowlInputRemapRule *self;

	g_return_val_if_fail(name != NULL && *name != '\0', NULL);

	self = g_new0(GowlInputRemapRule, 1);
	self->ref_count = 1;
	self->name = g_strdup(name);
	self->match_type = GOWL_INPUT_REMAP_DEVICE_ANY;
	self->map = g_hash_table_new_full(g_direct_hash, g_direct_equal,
	                                  NULL, target_free);
	return self;
}

/**
 * gowl_input_remap_rule_ref:
 * @self: a #GowlInputRemapRule
 *
 * Returns: (transfer full): @self, with one more reference
 */
GowlInputRemapRule *
gowl_input_remap_rule_ref(GowlInputRemapRule *self)
{
	g_return_val_if_fail(self != NULL, NULL);
	g_atomic_int_inc(&self->ref_count);
	return self;
}

/**
 * gowl_input_remap_rule_unref:
 * @self: (nullable): a #GowlInputRemapRule
 *
 * Drops a reference; the last one frees the rule, its targets, and
 * each callback's user data through its destroy notify.
 */
void
gowl_input_remap_rule_unref(GowlInputRemapRule *self)
{
	if (self == NULL)
		return;
	if (!g_atomic_int_dec_and_test(&self->ref_count))
		return;
	g_hash_table_unref(self->map);
	g_free(self->name);
	g_free(self->match_name);
	g_free(self->match_sysname);
	g_free(self);
}

/**
 * gowl_input_remap_rule_get_name:
 * @self: a #GowlInputRemapRule
 *
 * Returns: (transfer none): the rule's name
 */
const gchar *
gowl_input_remap_rule_get_name(GowlInputRemapRule *self)
{
	g_return_val_if_fail(self != NULL, NULL);
	return self->name;
}

/* --- Matching --- */

/**
 * gowl_input_remap_rule_set_match_name:
 * @self: a #GowlInputRemapRule
 * @glob: (nullable): a glob matched against the libinput device name,
 *   or %NULL to stop matching on the name
 */
void
gowl_input_remap_rule_set_match_name(
	GowlInputRemapRule *self,
	const gchar        *glob
){
	g_return_if_fail(self != NULL);
	g_free(self->match_name);
	self->match_name = (glob != NULL && *glob != '\0') ? g_strdup(glob) : NULL;
}

/**
 * gowl_input_remap_rule_get_match_name:
 * @self: a #GowlInputRemapRule
 *
 * Returns: (transfer none) (nullable): the device-name glob
 */
const gchar *
gowl_input_remap_rule_get_match_name(GowlInputRemapRule *self)
{
	g_return_val_if_fail(self != NULL, NULL);
	return self->match_name;
}

/**
 * gowl_input_remap_rule_set_match_sysname:
 * @self: a #GowlInputRemapRule
 * @glob: (nullable): a glob matched against the kernel sysname
 *   ("event*"), or %NULL
 */
void
gowl_input_remap_rule_set_match_sysname(
	GowlInputRemapRule *self,
	const gchar        *glob
){
	g_return_if_fail(self != NULL);
	g_free(self->match_sysname);
	self->match_sysname = (glob != NULL && *glob != '\0')
		? g_strdup(glob) : NULL;
}

/**
 * gowl_input_remap_rule_get_match_sysname:
 * @self: a #GowlInputRemapRule
 *
 * Returns: (transfer none) (nullable): the sysname glob
 */
const gchar *
gowl_input_remap_rule_get_match_sysname(GowlInputRemapRule *self)
{
	g_return_val_if_fail(self != NULL, NULL);
	return self->match_sysname;
}

/**
 * gowl_input_remap_rule_set_match_ids:
 * @self: a #GowlInputRemapRule
 * @vendor: the USB/HID vendor id, 0 for any
 * @product: the USB/HID product id, 0 for any
 *
 * Matches on the hardware ids libinput reports.  The most specific
 * criterion, and the one to prefer: two pedals of the same model share
 * a name but so, often, do unrelated devices from one vendor.
 */
void
gowl_input_remap_rule_set_match_ids(
	GowlInputRemapRule *self,
	guint16             vendor,
	guint16             product
){
	g_return_if_fail(self != NULL);
	self->vendor = vendor;
	self->product = product;
}

/**
 * gowl_input_remap_rule_get_match_vendor:
 * @self: a #GowlInputRemapRule
 *
 * Returns: the vendor id matched on, 0 for any
 */
guint16
gowl_input_remap_rule_get_match_vendor(GowlInputRemapRule *self)
{
	g_return_val_if_fail(self != NULL, 0);
	return self->vendor;
}

/**
 * gowl_input_remap_rule_get_match_product:
 * @self: a #GowlInputRemapRule
 *
 * Returns: the product id matched on, 0 for any
 */
guint16
gowl_input_remap_rule_get_match_product(GowlInputRemapRule *self)
{
	g_return_val_if_fail(self != NULL, 0);
	return self->product;
}

/**
 * gowl_input_remap_rule_set_match_type:
 * @self: a #GowlInputRemapRule
 * @type: the device kind to match; %GOWL_INPUT_REMAP_DEVICE_ANY
 *   matches both.  Not a match criterion on its own.
 */
void
gowl_input_remap_rule_set_match_type(
	GowlInputRemapRule       *self,
	GowlInputRemapDeviceType  type
){
	g_return_if_fail(self != NULL);
	self->match_type = type;
}

/**
 * gowl_input_remap_rule_get_match_type:
 * @self: a #GowlInputRemapRule
 *
 * Returns: the device kind matched
 */
GowlInputRemapDeviceType
gowl_input_remap_rule_get_match_type(GowlInputRemapRule *self)
{
	g_return_val_if_fail(self != NULL, GOWL_INPUT_REMAP_DEVICE_ANY);
	return self->match_type;
}

/**
 * gowl_input_remap_rule_has_match:
 * @self: a #GowlInputRemapRule
 *
 * Whether the rule names at least one of: a device-name glob, a
 * sysname glob, a vendor id or a product id.  A rule without one
 * matches nothing -- a rule that matched everything would claim the
 * keyboard you are typing on.
 *
 * Returns: %TRUE when the rule can match a device
 */
gboolean
gowl_input_remap_rule_has_match(GowlInputRemapRule *self)
{
	g_return_val_if_fail(self != NULL, FALSE);
	return self->match_name != NULL || self->match_sysname != NULL
	       || self->vendor != 0 || self->product != 0;
}

/**
 * gowl_input_remap_rule_matches:
 * @self: a #GowlInputRemapRule
 * @info: a connected device's identity
 *
 * Every criterion the rule sets must hold; criteria it leaves unset are
 * ignored.  Globs use g_pattern_match_simple(), so `*' and `?' only.
 *
 * Returns: %TRUE when @info is one of the rule's devices
 */
gboolean
gowl_input_remap_rule_matches(
	GowlInputRemapRule        *self,
	const GowlInputDeviceInfo *info
){
	g_return_val_if_fail(self != NULL, FALSE);
	g_return_val_if_fail(info != NULL, FALSE);

	if (!gowl_input_remap_rule_has_match(self))
		return FALSE;
	if (self->match_type != GOWL_INPUT_REMAP_DEVICE_ANY
	    && self->match_type != info->type)
		return FALSE;
	if (self->vendor != 0 && self->vendor != info->vendor)
		return FALSE;
	if (self->product != 0 && self->product != info->product)
		return FALSE;
	if (self->match_name != NULL
	    && (info->name == NULL
	        || !g_pattern_match_simple(self->match_name, info->name)))
		return FALSE;
	if (self->match_sysname != NULL
	    && (info->sysname == NULL
	        || !g_pattern_match_simple(self->match_sysname, info->sysname)))
		return FALSE;
	return TRUE;
}

/* --- Behaviour --- */

/**
 * gowl_input_remap_rule_set_drop_unmatched:
 * @self: a #GowlInputRemapRule
 * @drop: %TRUE to swallow every input the rule does not map; %FALSE
 *   (the default) to pass those through unchanged
 */
void
gowl_input_remap_rule_set_drop_unmatched(
	GowlInputRemapRule *self,
	gboolean            drop
){
	g_return_if_fail(self != NULL);
	self->drop_unmatched = drop ? TRUE : FALSE;
}

/**
 * gowl_input_remap_rule_get_drop_unmatched:
 * @self: a #GowlInputRemapRule
 *
 * Returns: whether unmapped inputs are swallowed
 */
gboolean
gowl_input_remap_rule_get_drop_unmatched(GowlInputRemapRule *self)
{
	g_return_val_if_fail(self != NULL, FALSE);
	return self->drop_unmatched;
}

/**
 * gowl_input_remap_rule_set_log:
 * @self: a #GowlInputRemapRule
 * @log: %TRUE to log every input this rule remaps, whatever the
 *   module's own `log' level
 */
void
gowl_input_remap_rule_set_log(
	GowlInputRemapRule *self,
	gboolean            log
){
	g_return_if_fail(self != NULL);
	self->log = log ? TRUE : FALSE;
}

/**
 * gowl_input_remap_rule_get_log:
 * @self: a #GowlInputRemapRule
 *
 * Returns: whether this rule's matches are always logged
 */
gboolean
gowl_input_remap_rule_get_log(GowlInputRemapRule *self)
{
	g_return_val_if_fail(self != NULL, FALSE);
	return self->log;
}

/* --- Mappings --- */

/**
 * gowl_input_remap_rule_map_key:
 * @self: a #GowlInputRemapRule
 * @input: the physical input (KEY_* / BTN_* / %GOWL_BUTTON_WHEEL_*)
 * @keycode: the evdev keycode to deliver, or 0 to use @keysym
 * @keysym: the keysym to deliver when @keycode is 0; resolved to a
 *   keycode against the live keymap at the moment of the press
 * @modifiers: #GowlKeyMod bits held for the key and released with it
 * @error: return location for a #GError
 *
 * Maps @input to ONE key.  The key goes through the whole compositor
 * key pipeline, so compositor keybinds, module keybinds and the embedder
 * see it before the focused client does.
 *
 * Returns: %TRUE on success
 */
gboolean
gowl_input_remap_rule_map_key(
	GowlInputRemapRule  *self,
	guint32              input,
	guint32              keycode,
	guint32              keysym,
	guint32              modifiers,
	GError             **error
){
	GowlInputRemapTarget *t;

	g_return_val_if_fail(self != NULL, FALSE);

	if (!check_input(input, error))
		return FALSE;
	if (keycode == 0 && keysym == XKB_KEY_NoSymbol) {
		g_set_error(error, GOWL_INPUT_REMAP_ERROR,
		            GOWL_INPUT_REMAP_ERROR_UNKNOWN_CODE,
		            "a key target needs a keycode or a keysym");
		return FALSE;
	}
	if (keycode > KEY_MAX || (keycode >= BTN_MISC && keycode < KEY_OK)) {
		g_set_error(error, GOWL_INPUT_REMAP_ERROR,
		            GOWL_INPUT_REMAP_ERROR_UNKNOWN_CODE,
		            "keycode 0x%x is not a keyboard key (a button "
		            "target is `button:')", keycode);
		return FALSE;
	}
	if ((modifiers & ~GOWL_INPUT_REMAP_MOD_MASK) != 0) {
		g_set_error(error, GOWL_INPUT_REMAP_ERROR,
		            GOWL_INPUT_REMAP_ERROR_INVALID,
		            "unsupported modifier bits 0x%x in a key target",
		            modifiers & ~GOWL_INPUT_REMAP_MOD_MASK);
		return FALSE;
	}

	t = g_new0(GowlInputRemapTarget, 1);
	t->kind = GOWL_INPUT_REMAP_TARGET_KEY;
	t->code = keycode;
	t->keysym = keycode != 0 ? XKB_KEY_NoSymbol : keysym;
	t->modifiers = modifiers;
	store_target(self, input, t);
	return TRUE;
}

/**
 * gowl_input_remap_rule_map_button:
 * @self: a #GowlInputRemapRule
 * @input: the physical input
 * @button: the BTN_* code to press at the cursor
 * @error: return location for a #GError
 *
 * Maps @input to ONE pointer button, pressed where the cursor is.  It
 * goes through the compositor's button path, so click-to-focus, the bar
 * and mousebinds all see it.
 *
 * Returns: %TRUE on success
 */
gboolean
gowl_input_remap_rule_map_button(
	GowlInputRemapRule  *self,
	guint32              input,
	guint32              button,
	GError             **error
){
	GowlInputRemapTarget *t;

	g_return_val_if_fail(self != NULL, FALSE);

	if (!check_input(input, error))
		return FALSE;
	if (button < BTN_MISC || button > KEY_MAX
	    || (button >= KEY_OK && button < BTN_DPAD_UP)) {
		g_set_error(error, GOWL_INPUT_REMAP_ERROR,
		            GOWL_INPUT_REMAP_ERROR_UNKNOWN_CODE,
		            "0x%x is not a button code", button);
		return FALSE;
	}

	t = g_new0(GowlInputRemapTarget, 1);
	t->kind = GOWL_INPUT_REMAP_TARGET_BUTTON;
	t->code = button;
	store_target(self, input, t);
	return TRUE;
}

/**
 * gowl_input_remap_rule_map_action:
 * @self: a #GowlInputRemapRule
 * @input: the physical input
 * @action: the compositor action, run on press
 * @arg: (nullable): its argument, as a keybind would carry it
 * @error: return location for a #GError
 *
 * Maps @input to ONE compositor action, run through the same dispatch
 * a keybind uses.  The release does nothing.
 *
 * Returns: %TRUE on success
 */
gboolean
gowl_input_remap_rule_map_action(
	GowlInputRemapRule  *self,
	guint32              input,
	GowlAction           action,
	const gchar         *arg,
	GError             **error
){
	GowlInputRemapTarget *t;
	GEnumClass *klass;
	gboolean known;

	g_return_val_if_fail(self != NULL, FALSE);

	if (!check_input(input, error))
		return FALSE;
	klass = (GEnumClass *)g_type_class_ref(GOWL_TYPE_ACTION);
	known = g_enum_get_value(klass, (gint)action) != NULL;
	g_type_class_unref(klass);
	if (!known || action == GOWL_ACTION_NONE) {
		g_set_error(error, GOWL_INPUT_REMAP_ERROR,
		            GOWL_INPUT_REMAP_ERROR_UNKNOWN_CODE,
		            "%d is not a compositor action", (gint)action);
		return FALSE;
	}

	t = g_new0(GowlInputRemapTarget, 1);
	t->kind = GOWL_INPUT_REMAP_TARGET_ACTION;
	t->action = action;
	t->arg = (arg != NULL && *arg != '\0') ? g_strdup(arg) : NULL;
	store_target(self, input, t);
	return TRUE;
}

/**
 * gowl_input_remap_rule_map_command:
 * @self: a #GowlInputRemapRule
 * @input: the physical input
 * @command: ONE command line for gowl_compositor_run_command(), run on
 *   press.  A newline is refused: one line is one command.
 * @error: return location for a #GError
 *
 * Returns: %TRUE on success
 */
gboolean
gowl_input_remap_rule_map_command(
	GowlInputRemapRule  *self,
	guint32              input,
	const gchar         *command,
	GError             **error
){
	GowlInputRemapTarget *t;

	g_return_val_if_fail(self != NULL, FALSE);

	if (!check_input(input, error))
		return FALSE;
	if (command == NULL || *command == '\0') {
		g_set_error(error, GOWL_INPUT_REMAP_ERROR,
		            GOWL_INPUT_REMAP_ERROR_INVALID,
		            "a command target needs a command");
		return FALSE;
	}
	if (strchr(command, '\n') != NULL || strchr(command, '\r') != NULL) {
		g_set_error(error, GOWL_INPUT_REMAP_ERROR,
		            GOWL_INPUT_REMAP_ERROR_NOT_ONE_TO_ONE,
		            "a command target is one line; several lines would "
		            "be several commands");
		return FALSE;
	}

	t = g_new0(GowlInputRemapTarget, 1);
	t->kind = GOWL_INPUT_REMAP_TARGET_COMMAND;
	t->arg = g_strdup(command);
	store_target(self, input, t);
	return TRUE;
}

/**
 * gowl_input_remap_rule_map_callback:
 * @self: a #GowlInputRemapRule
 * @input: the physical input
 * @callback: (scope notified) (closure user_data) (destroy destroy):
 *   called on press and on release
 * @user_data: (nullable): passed to @callback
 * @destroy: (nullable): frees @user_data with the rule
 *
 * Maps @input to arbitrary code.  This is the escape hatch: whatever
 * @callback does is its own business, and the one-to-one guarantee of
 * the other targets does not extend to it.
 */
void
gowl_input_remap_rule_map_callback(
	GowlInputRemapRule     *self,
	guint32                 input,
	GowlInputRemapCallback  callback,
	gpointer                user_data,
	GDestroyNotify          destroy
){
	GowlInputRemapTarget *t;

	g_return_if_fail(self != NULL);
	g_return_if_fail(callback != NULL);
	g_return_if_fail(input_is_valid(input));

	t = g_new0(GowlInputRemapTarget, 1);
	t->kind = GOWL_INPUT_REMAP_TARGET_CALLBACK;
	t->callback = callback;
	t->user_data = user_data;
	t->destroy = destroy;
	store_target(self, input, t);
	g_atomic_int_inc((gint *)&callback_generation);
}

/**
 * gowl_input_remap_rule_map_drop:
 * @self: a #GowlInputRemapRule
 * @input: the physical input to swallow
 */
void
gowl_input_remap_rule_map_drop(
	GowlInputRemapRule *self,
	guint32             input
){
	GowlInputRemapTarget *t;

	g_return_if_fail(self != NULL);
	g_return_if_fail(input_is_valid(input));

	t = g_new0(GowlInputRemapTarget, 1);
	t->kind = GOWL_INPUT_REMAP_TARGET_DROP;
	store_target(self, input, t);
}

/**
 * gowl_input_remap_rule_map_pass:
 * @self: a #GowlInputRemapRule
 * @input: the physical input to deliver unchanged
 *
 * Only meaningful on a rule that drops unmatched inputs: it lets one
 * input through regardless.
 */
void
gowl_input_remap_rule_map_pass(
	GowlInputRemapRule *self,
	guint32             input
){
	GowlInputRemapTarget *t;

	g_return_if_fail(self != NULL);
	g_return_if_fail(input_is_valid(input));

	t = g_new0(GowlInputRemapTarget, 1);
	t->kind = GOWL_INPUT_REMAP_TARGET_PASS;
	store_target(self, input, t);
}

/**
 * gowl_input_remap_rule_unmap:
 * @self: a #GowlInputRemapRule
 * @input: the physical input
 *
 * Returns: %TRUE when @input had a mapping
 */
gboolean
gowl_input_remap_rule_unmap(
	GowlInputRemapRule *self,
	guint32             input
){
	g_return_val_if_fail(self != NULL, FALSE);
	return g_hash_table_remove(self->map, GUINT_TO_POINTER(input));
}

/**
 * gowl_input_remap_rule_get_n_mappings:
 * @self: a #GowlInputRemapRule
 *
 * Returns: the number of inputs the rule maps
 */
guint
gowl_input_remap_rule_get_n_mappings(GowlInputRemapRule *self)
{
	g_return_val_if_fail(self != NULL, 0);
	return g_hash_table_size(self->map);
}

static gint
compare_guint32(
	gconstpointer a,
	gconstpointer b
){
	guint32 x;
	guint32 y;

	x = *(const guint32 *)a;
	y = *(const guint32 *)b;
	return (x > y) - (x < y);
}

/**
 * gowl_input_remap_rule_get_inputs:
 * @self: a #GowlInputRemapRule
 *
 * Returns: (transfer full) (element-type guint32): every mapped input,
 *   in ascending order
 */
GArray *
gowl_input_remap_rule_get_inputs(GowlInputRemapRule *self)
{
	GHashTableIter iter;
	gpointer key;
	GArray *out;

	g_return_val_if_fail(self != NULL, NULL);

	out = g_array_sized_new(FALSE, FALSE, sizeof(guint32),
	                        g_hash_table_size(self->map));
	g_hash_table_iter_init(&iter, self->map);
	while (g_hash_table_iter_next(&iter, &key, NULL)) {
		guint32 code;

		code = GPOINTER_TO_UINT(key);
		g_array_append_val(out, code);
	}
	g_array_sort(out, compare_guint32);
	return out;
}

/**
 * gowl_input_remap_rule_lookup:
 * @self: a #GowlInputRemapRule
 * @input: the physical input
 *
 * Returns: (transfer none) (nullable): the target @input maps to, or
 *   %NULL when the rule does not mention it
 */
const GowlInputRemapTarget *
gowl_input_remap_rule_lookup(
	GowlInputRemapRule *self,
	guint32             input
){
	g_return_val_if_fail(self != NULL, NULL);
	return (const GowlInputRemapTarget *)g_hash_table_lookup(
		self->map, GUINT_TO_POINTER(input));
}

/* --- Targets --- */

/**
 * gowl_input_remap_target_get_kind:
 * @self: a #GowlInputRemapTarget
 *
 * Returns: what the input turns into
 */
GowlInputRemapTargetKind
gowl_input_remap_target_get_kind(const GowlInputRemapTarget *self)
{
	g_return_val_if_fail(self != NULL, GOWL_INPUT_REMAP_TARGET_PASS);
	return self->kind;
}

/**
 * gowl_input_remap_target_get_code:
 * @self: a #GowlInputRemapTarget
 *
 * Returns: the evdev keycode of a key target (0 when it names a
 *   keysym), or the BTN_* code of a button target
 */
guint32
gowl_input_remap_target_get_code(const GowlInputRemapTarget *self)
{
	g_return_val_if_fail(self != NULL, 0);
	return self->code;
}

/**
 * gowl_input_remap_target_get_keysym:
 * @self: a #GowlInputRemapTarget
 *
 * Returns: the keysym of a key target, or 0
 */
guint32
gowl_input_remap_target_get_keysym(const GowlInputRemapTarget *self)
{
	g_return_val_if_fail(self != NULL, 0);
	return self->keysym;
}

/**
 * gowl_input_remap_target_get_modifiers:
 * @self: a #GowlInputRemapTarget
 *
 * Returns: the #GowlKeyMod bits a key target holds
 */
guint32
gowl_input_remap_target_get_modifiers(const GowlInputRemapTarget *self)
{
	g_return_val_if_fail(self != NULL, 0);
	return self->modifiers;
}

/**
 * gowl_input_remap_target_get_action:
 * @self: a #GowlInputRemapTarget
 *
 * Returns: the action of an action target, else %GOWL_ACTION_NONE
 */
GowlAction
gowl_input_remap_target_get_action(const GowlInputRemapTarget *self)
{
	g_return_val_if_fail(self != NULL, GOWL_ACTION_NONE);
	return self->action;
}

/**
 * gowl_input_remap_target_get_arg:
 * @self: a #GowlInputRemapTarget
 *
 * Returns: (transfer none) (nullable): an action's argument, or a
 *   command target's command line
 */
const gchar *
gowl_input_remap_target_get_arg(const GowlInputRemapTarget *self)
{
	g_return_val_if_fail(self != NULL, NULL);
	return self->arg;
}

/**
 * gowl_input_remap_target_invoke:
 * @self: a callback target
 * @rule: the rule it belongs to
 * @event: the physical input
 * @compositor: (nullable): the compositor
 *
 * Calls a callback target's function.  Does nothing for any other kind.
 */
void
gowl_input_remap_target_invoke(
	const GowlInputRemapTarget *self,
	GowlInputRemapRule         *rule,
	const GowlInputRemapEvent  *event,
	GowlCompositor             *compositor
){
	g_return_if_fail(self != NULL);
	g_return_if_fail(event != NULL);

	if (self->kind != GOWL_INPUT_REMAP_TARGET_CALLBACK
	    || self->callback == NULL)
		return;
	self->callback(rule, event, compositor, self->user_data);
}

/* Appends "Super+Shift+" for @mods. */
static void
append_modifiers(
	GString *out,
	guint32  mods
){
	if (mods & GOWL_KEY_MOD_LOGO)
		g_string_append(out, "Super+");
	if (mods & GOWL_KEY_MOD_CTRL)
		g_string_append(out, "Ctrl+");
	if (mods & GOWL_KEY_MOD_ALT)
		g_string_append(out, "Alt+");
	if (mods & GOWL_KEY_MOD_SHIFT)
		g_string_append(out, "Shift+");
	if (mods & GOWL_KEY_MOD_MOD3)
		g_string_append(out, "Mod3+");
	if (mods & GOWL_KEY_MOD_MOD5)
		g_string_append(out, "Mod5+");
}

/* A YAML double-quoted scalar.  Only `\' and `"' need escaping in the
 * strings rules carry; control characters are refused on the way in
 * (a command is one line) and never reach here. */
static void
append_quoted(
	GString     *out,
	const gchar *s
){
	const gchar *p;

	g_string_append_c(out, '"');
	for (p = s; p != NULL && *p != '\0'; p++) {
		if (*p == '"' || *p == '\\')
			g_string_append_c(out, '\\');
		g_string_append_c(out, *p);
	}
	g_string_append_c(out, '"');
}

/* The value half of a map entry, in the YAML form the loader reads. */
static void
append_target_yaml(
	GString                    *out,
	const GowlInputRemapTarget *t
){
	switch (t->kind) {
	case GOWL_INPUT_REMAP_TARGET_PASS:
		g_string_append(out, "pass");
		return;
	case GOWL_INPUT_REMAP_TARGET_DROP:
		g_string_append(out, "drop");
		return;
	case GOWL_INPUT_REMAP_TARGET_KEY: {
		g_autoptr(GString) key = g_string_new(NULL);

		append_modifiers(key, t->modifiers);
		if (t->code != 0) {
			g_autofree gchar *n = gowl_input_remap_code_to_name(t->code);

			g_string_append(key, n);
		} else {
			gchar buf[64];

			xkb_keysym_get_name((xkb_keysym_t)t->keysym, buf, sizeof buf);
			g_string_append(key, buf);
		}
		g_string_append(out, "{key: ");
		append_quoted(out, key->str);
		g_string_append_c(out, '}');
		return;
	}
	case GOWL_INPUT_REMAP_TARGET_BUTTON: {
		g_autofree gchar *n = gowl_input_remap_code_to_name(t->code);

		g_string_append_printf(out, "{button: %s}", n);
		return;
	}
	case GOWL_INPUT_REMAP_TARGET_ACTION: {
		GEnumClass *klass;
		GEnumValue *val;

		klass = (GEnumClass *)g_type_class_ref(GOWL_TYPE_ACTION);
		val = g_enum_get_value(klass, (gint)t->action);
		g_string_append_printf(out, "{action: %s",
		                       val != NULL ? val->value_nick : "none");
		g_type_class_unref(klass);
		if (t->arg != NULL) {
			g_string_append(out, ", arg: ");
			append_quoted(out, t->arg);
		}
		g_string_append_c(out, '}');
		return;
	}
	case GOWL_INPUT_REMAP_TARGET_COMMAND:
		g_string_append(out, "{command: ");
		append_quoted(out, t->arg);
		g_string_append_c(out, '}');
		return;
	case GOWL_INPUT_REMAP_TARGET_CALLBACK:
	default:
		/* Code has no text form.  Written so a listing shows it;
		 * the loader refuses it, since only C can register one. */
		g_string_append(out, "{callback: native}");
		return;
	}
}

/**
 * gowl_input_remap_target_to_string:
 * @self: a #GowlInputRemapTarget
 *
 * Returns: (transfer full): the target in the YAML form a rule's `map:'
 *   uses, e.g. `{button: BTN_MIDDLE}'
 */
gchar *
gowl_input_remap_target_to_string(const GowlInputRemapTarget *self)
{
	GString *out;

	g_return_val_if_fail(self != NULL, NULL);

	out = g_string_new(NULL);
	append_target_yaml(out, self);
	return g_string_free(out, FALSE);
}

/**
 * gowl_input_remap_rule_to_yaml:
 * @self: a #GowlInputRemapRule
 *
 * The rule as one line of flow-style YAML, which
 * gowl_input_remap_rule_new_from_yaml() reads back (callback targets
 * excepted: code has no text form).  Also what `inputremap-list'
 * prints.
 *
 * Returns: (transfer full): the YAML text
 */
gchar *
gowl_input_remap_rule_to_yaml(GowlInputRemapRule *self)
{
	g_autoptr(GArray) inputs = NULL;
	GString *out;
	gboolean first;
	guint i;

	g_return_val_if_fail(self != NULL, NULL);

	out = g_string_new("{name: ");
	append_quoted(out, self->name);

	/* match: only the criteria that are set */
	g_string_append(out, ", match: {");
	first = TRUE;
	if (self->match_name != NULL) {
		g_string_append(out, "name: ");
		append_quoted(out, self->match_name);
		first = FALSE;
	}
	if (self->vendor != 0) {
		g_string_append_printf(out, "%svendor: 0x%04x",
		                       first ? "" : ", ", (guint)self->vendor);
		first = FALSE;
	}
	if (self->product != 0) {
		g_string_append_printf(out, "%sproduct: 0x%04x",
		                       first ? "" : ", ", (guint)self->product);
		first = FALSE;
	}
	if (self->match_sysname != NULL) {
		g_string_append(out, first ? "sysname: " : ", sysname: ");
		append_quoted(out, self->match_sysname);
		first = FALSE;
	}
	if (self->match_type != GOWL_INPUT_REMAP_DEVICE_ANY)
		g_string_append_printf(out, "%stype: %s", first ? "" : ", ",
		                       self->match_type == GOWL_INPUT_REMAP_DEVICE_KEYBOARD
		                       ? "keyboard" : "pointer");
	g_string_append_c(out, '}');

	if (self->drop_unmatched)
		g_string_append(out, ", unmatched: drop");
	if (self->log)
		g_string_append(out, ", log: true");

	/* map: in input order, so two listings of one rule are equal */
	g_string_append(out, ", map: {");
	inputs = gowl_input_remap_rule_get_inputs(self);
	for (i = 0; i < inputs->len; i++) {
		guint32 code;
		g_autofree gchar *name = NULL;

		code = g_array_index(inputs, guint32, i);
		name = gowl_input_remap_code_to_name(code);
		g_string_append_printf(out, "%s%s: ", i == 0 ? "" : ", ", name);
		append_target_yaml(out, gowl_input_remap_rule_lookup(self, code));
	}
	g_string_append(out, "}}");
	return g_string_free(out, FALSE);
}

/* --- Names --- */

static gboolean
lookup_table(
	const GowlEvdevName *table,
	const gchar         *name,
	gboolean             ignore_case,
	guint32             *out
){
	const GowlEvdevName *e;

	for (e = table; e->name != NULL; e++) {
		if (ignore_case ? g_ascii_strcasecmp(e->name, name) == 0
		                : strcmp(e->name, name) == 0) {
			*out = e->code;
			return TRUE;
		}
	}
	return FALSE;
}

/* A number in decimal or 0x-hex, the whole string. */
static gboolean
parse_number(
	const gchar *s,
	guint64      max,
	guint64     *out
){
	gchar *end;
	guint64 v;

	if (s == NULL || *s == '\0' || !g_ascii_isdigit(*s))
		return FALSE;
	v = g_ascii_strtoull(s, &end, 0);
	if (*end != '\0' || v > max)
		return FALSE;
	*out = v;
	return TRUE;
}

/**
 * gowl_input_remap_code_from_name:
 * @name: an input name
 * @out_code: (out): the input code
 *
 * Accepts evdev names (`KEY_A', `BTN_SIDE', any the kernel headers
 * define, case-insensitive), the friendly button names (`left',
 * `middle', `side', `extra', `forward', `back', `task'), wheel notches
 * (`WHEEL_UP', `WHEEL_DOWN', `WHEEL_LEFT', `WHEEL_RIGHT') and raw
 * numeric codes (`30', `0x110').
 *
 * Returns: %TRUE when @name resolved
 */
gboolean
gowl_input_remap_code_from_name(
	const gchar *name,
	guint32     *out_code
){
	g_autofree gchar *norm = NULL;
	guint32 code;
	guint64 n;

	g_return_val_if_fail(out_code != NULL, FALSE);

	if (name == NULL || *name == '\0')
		return FALSE;

	/* `wheel-up' reads as naturally as WHEEL_UP */
	norm = g_ascii_strup(name, -1);
	g_strdelimit(norm, "-", '_');

	if (lookup_table(evdev_names, norm, FALSE, &code)
	    || lookup_table(wheel_names, norm, FALSE, &code)
	    || lookup_table(button_names, name, TRUE, &code)) {
		*out_code = code;
		return TRUE;
	}
	if (parse_number(name, G_MAXUINT32, &n) && input_is_valid((guint32)n)) {
		*out_code = (guint32)n;
		return TRUE;
	}
	return FALSE;
}

/**
 * gowl_input_remap_code_to_name:
 * @code: an input code
 *
 * Returns: (transfer full): the evdev or wheel name for @code, or its
 *   hex value when it has none
 */
gchar *
gowl_input_remap_code_to_name(guint32 code)
{
	const GowlEvdevName *e;

	for (e = wheel_names; e->name != NULL; e++)
		if (e->code == code)
			return g_strdup(e->name);
	for (e = evdev_names; e->name != NULL; e++)
		if (e->code == code)
			return g_strdup(e->name);
	return g_strdup_printf("0x%x", code);
}

/**
 * gowl_input_remap_button_from_name:
 * @name: a button name
 * @out_button: (out): the BTN_* code
 *
 * Accepts the friendly names (`left', `middle', `right', `side',
 * `extra', `forward', `back', `task', `button1'..`button3', `button8',
 * `button9'), any `BTN_*' name and numeric codes.
 *
 * Returns: %TRUE when @name is a button
 */
gboolean
gowl_input_remap_button_from_name(
	const gchar *name,
	guint32     *out_button
){
	g_autofree gchar *norm = NULL;
	guint32 code;
	guint64 n;

	g_return_val_if_fail(out_button != NULL, FALSE);

	if (name == NULL || *name == '\0')
		return FALSE;
	if (lookup_table(button_names, name, TRUE, &code)) {
		*out_button = code;
		return TRUE;
	}
	norm = g_ascii_strup(name, -1);
	if (g_str_has_prefix(norm, "BTN_")
	    && lookup_table(evdev_names, norm, FALSE, &code)) {
		*out_button = code;
		return TRUE;
	}
	if (parse_number(name, KEY_MAX, &n) && n >= BTN_MISC) {
		*out_button = (guint32)n;
		return TRUE;
	}
	return FALSE;
}

/* --- YAML loading --- */

static gboolean
key_is_forbidden(const gchar *key)
{
	return key != NULL && g_strv_contains(forbidden_keys, key);
}

/* A scalar's text, or NULL for a non-scalar. */
static const gchar *
node_scalar(YamlNode *node)
{
	if (node == NULL)
		return NULL;
	if (yaml_node_get_node_type(node) != YAML_NODE_SCALAR)
		return NULL;
	return yaml_node_get_scalar(node);
}

/* Refuses a sequence where one value belongs.  A list is exactly the
 * shape a macro takes, so it gets the one-to-one error, not a generic
 * type error. */
static gboolean
require_scalar(
	YamlNode     *node,
	const gchar  *what,
	const gchar **out,
	GError      **error
){
	if (node != NULL && yaml_node_get_node_type(node) == YAML_NODE_SEQUENCE) {
		g_set_error(error, GOWL_INPUT_REMAP_ERROR,
		            GOWL_INPUT_REMAP_ERROR_NOT_ONE_TO_ONE,
		            "`%s' is a list; one input maps to one output, "
		            "never a sequence", what);
		return FALSE;
	}
	*out = node_scalar(node);
	if (*out == NULL || **out == '\0') {
		g_set_error(error, GOWL_INPUT_REMAP_ERROR,
		            GOWL_INPUT_REMAP_ERROR_INVALID,
		            "`%s' needs a value", what);
		return FALSE;
	}
	return TRUE;
}

/*
 * A key target: "Super+Shift+9", "Page_Up", "KEY_F13", "Ctrl+KEY_C".
 * Modifier tokens first, then exactly one key.  The last token is an
 * evdev KEY_* name if it is one, else a keysym name.
 */
static gboolean
parse_key_target(
	const gchar  *text,
	guint32      *out_keycode,
	guint32      *out_keysym,
	guint32      *out_mods,
	GError      **error
){
	g_auto(GStrv) tokens = NULL;
	guint n;
	guint i;
	guint32 mods;
	guint32 code;
	g_autofree gchar *up = NULL;
	xkb_keysym_t sym;
	const gchar *last;

	tokens = g_strsplit(text, "+", -1);
	n = g_strv_length(tokens);
	if (n == 0) {
		g_set_error(error, GOWL_INPUT_REMAP_ERROR,
		            GOWL_INPUT_REMAP_ERROR_INVALID, "empty key target");
		return FALSE;
	}

	/* Every token but the last is a modifier */
	mods = 0;
	for (i = 0; i + 1 < n; i++) {
		guint32 m;

		g_strstrip(tokens[i]);
		if (!lookup_table(modifier_names, tokens[i], TRUE, &m)) {
			g_set_error(error, GOWL_INPUT_REMAP_ERROR,
			            GOWL_INPUT_REMAP_ERROR_NOT_ONE_TO_ONE,
			            "`%s' in key target \"%s\" is not a modifier; "
			            "a key target is modifiers and ONE key",
			            tokens[i], text);
			return FALSE;
		}
		mods |= m;
	}

	last = g_strstrip(tokens[n - 1]);
	if (*last == '\0' || strchr(last, ' ') != NULL
	    || strchr(last, ',') != NULL) {
		g_set_error(error, GOWL_INPUT_REMAP_ERROR,
		            GOWL_INPUT_REMAP_ERROR_NOT_ONE_TO_ONE,
		            "key target \"%s\" is not a single key", text);
		return FALSE;
	}

	/* KEY_* evdev name: layout-independent */
	up = g_ascii_strup(last, -1);
	if (g_str_has_prefix(up, "KEY_")
	    && lookup_table(evdev_names, up, FALSE, &code)) {
		*out_keycode = code;
		*out_keysym = XKB_KEY_NoSymbol;
		*out_mods = mods;
		return TRUE;
	}

	/* Otherwise a keysym, resolved against the live keymap on press */
	sym = xkb_keysym_from_name(last, XKB_KEYSYM_NO_FLAGS);
	if (sym == XKB_KEY_NoSymbol)
		sym = xkb_keysym_from_name(last, XKB_KEYSYM_CASE_INSENSITIVE);
	if (sym == XKB_KEY_NoSymbol) {
		g_set_error(error, GOWL_INPUT_REMAP_ERROR,
		            GOWL_INPUT_REMAP_ERROR_UNKNOWN_CODE,
		            "`%s' is neither a KEY_* name nor a keysym", last);
		return FALSE;
	}
	*out_keycode = 0;
	*out_keysym = (guint32)sym;
	*out_mods = mods;
	return TRUE;
}

/* The action nick, forgiving `_' for `-' as `action NAME' over IPC is. */
static gboolean
parse_action(
	const gchar  *text,
	GowlAction   *out,
	GError      **error
){
	g_autofree gchar *norm = NULL;
	GEnumClass *klass;
	GEnumValue *val;

	norm = g_ascii_strdown(text, -1);
	g_strdelimit(norm, "_", '-');
	klass = (GEnumClass *)g_type_class_ref(GOWL_TYPE_ACTION);
	val = g_enum_get_value_by_nick(klass, norm);
	if (val != NULL)
		*out = (GowlAction)val->value;
	g_type_class_unref(klass);
	if (val == NULL) {
		g_set_error(error, GOWL_INPUT_REMAP_ERROR,
		            GOWL_INPUT_REMAP_ERROR_UNKNOWN_CODE,
		            "`%s' is not a compositor action", text);
		return FALSE;
	}
	return TRUE;
}

/*
 * One `map:' entry.  The value is `drop', `pass', or a mapping holding
 * EXACTLY ONE of key / button / action / command (plus `arg' with an
 * action).  Anything that would yield a second output is refused with
 * NOT_ONE_TO_ONE.
 */
static gboolean
parse_map_entry(
	GowlInputRemapRule  *rule,
	const gchar         *input_name,
	YamlNode            *value,
	GError             **error
){
	YamlMapping *m;
	guint32 input;
	guint n;
	guint i;
	guint kinds;
	const gchar *text;

	if (!gowl_input_remap_code_from_name(input_name, &input)) {
		g_set_error(error, GOWL_INPUT_REMAP_ERROR,
		            GOWL_INPUT_REMAP_ERROR_UNKNOWN_CODE,
		            "`%s' is not an input name (KEY_*, BTN_*, left, "
		            "middle, WHEEL_UP, ...)", input_name);
		return FALSE;
	}
	if (value == NULL) {
		g_set_error(error, GOWL_INPUT_REMAP_ERROR,
		            GOWL_INPUT_REMAP_ERROR_INVALID,
		            "`%s' maps to nothing", input_name);
		return FALSE;
	}

	/* A list of targets is a sequence -- the thing this refuses */
	if (yaml_node_get_node_type(value) == YAML_NODE_SEQUENCE) {
		g_set_error(error, GOWL_INPUT_REMAP_ERROR,
		            GOWL_INPUT_REMAP_ERROR_NOT_ONE_TO_ONE,
		            "`%s' maps to a list; one input maps to one output",
		            input_name);
		return FALSE;
	}

	/* The two scalar forms */
	text = node_scalar(value);
	if (text != NULL) {
		if (g_ascii_strcasecmp(text, "drop") == 0) {
			gowl_input_remap_rule_map_drop(rule, input);
			return TRUE;
		}
		if (g_ascii_strcasecmp(text, "pass") == 0) {
			gowl_input_remap_rule_map_pass(rule, input);
			return TRUE;
		}
		g_set_error(error, GOWL_INPUT_REMAP_ERROR,
		            GOWL_INPUT_REMAP_ERROR_INVALID,
		            "`%s: %s' -- a bare value must be drop or pass; "
		            "anything else is a mapping like {key: ...}",
		            input_name, text);
		return FALSE;
	}

	m = yaml_node_get_mapping(value);
	if (m == NULL) {
		g_set_error(error, GOWL_INPUT_REMAP_ERROR,
		            GOWL_INPUT_REMAP_ERROR_INVALID,
		            "`%s' must map to drop, pass or a mapping",
		            input_name);
		return FALSE;
	}

	/* Vet every key before acting on any */
	n = yaml_mapping_get_size(m);
	kinds = 0;
	for (i = 0; i < n; i++) {
		const gchar *k;

		k = yaml_mapping_get_key(m, i);
		if (key_is_forbidden(k)) {
			g_set_error(error, GOWL_INPUT_REMAP_ERROR,
			            GOWL_INPUT_REMAP_ERROR_NOT_ONE_TO_ONE,
			            "`%s' in the mapping for %s: remaps are one "
			            "press to one output -- no sequences, macros, "
			            "delays or repeats", k, input_name);
			return FALSE;
		}
		if (g_strcmp0(k, "key") == 0 || g_strcmp0(k, "button") == 0
		    || g_strcmp0(k, "action") == 0
		    || g_strcmp0(k, "command") == 0)
			kinds++;
		else if (g_strcmp0(k, "callback") == 0) {
			g_set_error(error, GOWL_INPUT_REMAP_ERROR,
			            GOWL_INPUT_REMAP_ERROR_INVALID,
			            "`callback' for %s: code can only be mapped "
			            "from C (gowl_input_remap_rule_map_callback) "
			            "or, in cmacs, from Elisp", input_name);
			return FALSE;
		} else if (g_strcmp0(k, "arg") != 0) {
			g_set_error(error, GOWL_INPUT_REMAP_ERROR,
			            GOWL_INPUT_REMAP_ERROR_INVALID,
			            "unknown key `%s' in the mapping for %s",
			            k, input_name);
			return FALSE;
		}
	}
	if (kinds > 1) {
		g_set_error(error, GOWL_INPUT_REMAP_ERROR,
		            GOWL_INPUT_REMAP_ERROR_NOT_ONE_TO_ONE,
		            "the mapping for %s names %u outputs; exactly one "
		            "of key, button, action or command", input_name,
		            kinds);
		return FALSE;
	}
	if (kinds == 0) {
		g_set_error(error, GOWL_INPUT_REMAP_ERROR,
		            GOWL_INPUT_REMAP_ERROR_INVALID,
		            "the mapping for %s names no output (key, button, "
		            "action or command)", input_name);
		return FALSE;
	}
	if (yaml_mapping_has_member(m, "arg")
	    && !yaml_mapping_has_member(m, "action")) {
		g_set_error(error, GOWL_INPUT_REMAP_ERROR,
		            GOWL_INPUT_REMAP_ERROR_INVALID,
		            "`arg' for %s only goes with `action'", input_name);
		return FALSE;
	}

	/* Exactly one kind is present: build it */
	if (yaml_mapping_has_member(m, "key")) {
		guint32 keycode;
		guint32 keysym;
		guint32 mods;

		if (!require_scalar(yaml_mapping_get_member(m, "key"), "key",
		                    &text, error))
			return FALSE;
		if (!parse_key_target(text, &keycode, &keysym, &mods, error))
			return FALSE;
		return gowl_input_remap_rule_map_key(rule, input, keycode, keysym,
		                                     mods, error);
	}
	if (yaml_mapping_has_member(m, "button")) {
		guint32 button;

		if (!require_scalar(yaml_mapping_get_member(m, "button"), "button",
		                    &text, error))
			return FALSE;
		if (!gowl_input_remap_button_from_name(text, &button)) {
			g_set_error(error, GOWL_INPUT_REMAP_ERROR,
			            GOWL_INPUT_REMAP_ERROR_UNKNOWN_CODE,
			            "`%s' is not a button (left, middle, right, "
			            "side, extra, BTN_*)", text);
			return FALSE;
		}
		return gowl_input_remap_rule_map_button(rule, input, button, error);
	}
	if (yaml_mapping_has_member(m, "action")) {
		GowlAction action;
		const gchar *arg;
		YamlNode *arg_node;

		if (!require_scalar(yaml_mapping_get_member(m, "action"), "action",
		                    &text, error))
			return FALSE;
		if (!parse_action(text, &action, error))
			return FALSE;
		arg = NULL;
		arg_node = yaml_mapping_get_member(m, "arg");
		if (arg_node != NULL && !require_scalar(arg_node, "arg", &arg, error))
			return FALSE;
		return gowl_input_remap_rule_map_action(rule, input, action, arg,
		                                        error);
	}
	if (!require_scalar(yaml_mapping_get_member(m, "command"), "command",
	                    &text, error))
		return FALSE;
	return gowl_input_remap_rule_map_command(rule, input, text, error);
}

/* A USB/HID id: one to four hex digits, optionally 0x-prefixed. */
static gboolean
parse_hex_id(
	const gchar *s,
	guint16     *out
){
	gsize len;

	if (g_str_has_prefix(s, "0x") || g_str_has_prefix(s, "0X"))
		s += 2;
	len = strlen(s);
	if (len == 0 || len > 4 || strspn(s, "0123456789abcdefABCDEF") != len)
		return FALSE;
	*out = (guint16)g_ascii_strtoull(s, NULL, 16);
	return TRUE;
}

/* The `match:' block. */
static gboolean
parse_match(
	GowlInputRemapRule  *rule,
	YamlMapping         *m,
	GError             **error
){
	guint n;
	guint i;

	n = yaml_mapping_get_size(m);
	for (i = 0; i < n; i++) {
		const gchar *k;
		const gchar *v;
		guint16 num;

		k = yaml_mapping_get_key(m, i);
		if (!require_scalar(yaml_mapping_get_value(m, i), k, &v, error))
			return FALSE;

		if (g_strcmp0(k, "name") == 0) {
			gowl_input_remap_rule_set_match_name(rule, v);
		} else if (g_strcmp0(k, "sysname") == 0) {
			gowl_input_remap_rule_set_match_sysname(rule, v);
		} else if (g_strcmp0(k, "vendor") == 0
		           || g_strcmp0(k, "product") == 0) {
			/* Always hex, with or without 0x: that is how lsusb and
			 * inputremap-devices print them, and a decimal reading
			 * of "1234" would silently name a different device. */
			if (!parse_hex_id(v, &num)) {
				g_set_error(error, GOWL_INPUT_REMAP_ERROR,
				            GOWL_INPUT_REMAP_ERROR_INVALID,
				            "`%s: %s' is not a 16-bit hex id", k, v);
				return FALSE;
			}
			if (g_strcmp0(k, "vendor") == 0)
				rule->vendor = num;
			else
				rule->product = num;
		} else if (g_strcmp0(k, "id") == 0) {
			/* "1a86:e026", as lsusb and inputremap-devices print it */
			guint vendor;
			guint product;
			gchar tail;

			if (sscanf(v, "%4x:%4x%c", &vendor, &product, &tail) != 2) {
				g_set_error(error, GOWL_INPUT_REMAP_ERROR,
				            GOWL_INPUT_REMAP_ERROR_INVALID,
				            "`id: %s' is not VENDOR:PRODUCT in hex", v);
				return FALSE;
			}
			gowl_input_remap_rule_set_match_ids(rule, (guint16)vendor,
			                                    (guint16)product);
		} else if (g_strcmp0(k, "type") == 0) {
			if (g_ascii_strcasecmp(v, "keyboard") == 0)
				rule->match_type = GOWL_INPUT_REMAP_DEVICE_KEYBOARD;
			else if (g_ascii_strcasecmp(v, "pointer") == 0
			         || g_ascii_strcasecmp(v, "mouse") == 0)
				rule->match_type = GOWL_INPUT_REMAP_DEVICE_POINTER;
			else if (g_ascii_strcasecmp(v, "any") == 0)
				rule->match_type = GOWL_INPUT_REMAP_DEVICE_ANY;
			else {
				g_set_error(error, GOWL_INPUT_REMAP_ERROR,
				            GOWL_INPUT_REMAP_ERROR_INVALID,
				            "`type: %s' -- keyboard, pointer or any", v);
				return FALSE;
			}
		} else {
			g_set_error(error, GOWL_INPUT_REMAP_ERROR,
			            GOWL_INPUT_REMAP_ERROR_INVALID,
			            "unknown match key `%s' (name, vendor, product, "
			            "id, sysname, type)", k);
			return FALSE;
		}
	}
	if (!gowl_input_remap_rule_has_match(rule)) {
		g_set_error(error, GOWL_INPUT_REMAP_ERROR,
		            GOWL_INPUT_REMAP_ERROR_INVALID,
		            "rule `%s' matches no device: give it a name, id, "
		            "vendor, product or sysname", rule->name);
		return FALSE;
	}
	return TRUE;
}

/**
 * gowl_input_remap_rule_new_from_node: (skip)
 * @node: a YAML mapping node describing one rule
 * @error: return location for a #GError
 *
 * The loader behind both the config section and the text entry points.
 * See docs/input-remap.org for the schema.
 *
 * Returns: (transfer full) (nullable): the rule, or %NULL with @error set
 */
GowlInputRemapRule *
gowl_input_remap_rule_new_from_node(
	YamlNode  *node,
	GError   **error
){
	g_autoptr(GowlInputRemapRule) rule = NULL;
	YamlMapping *m;
	YamlMapping *match;
	YamlMapping *map;
	const gchar *name;
	const gchar *v;
	guint n;
	guint i;

	if (node == NULL || yaml_node_get_node_type(node) != YAML_NODE_MAPPING) {
		g_set_error(error, GOWL_INPUT_REMAP_ERROR,
		            GOWL_INPUT_REMAP_ERROR_INVALID,
		            "a remap rule is a mapping with name, match and map");
		return NULL;
	}
	m = yaml_node_get_mapping(node);

	/* Top-level keys first, so a macro-shaped rule is refused as that
	 * and not as whatever else happens to be wrong with it. */
	n = yaml_mapping_get_size(m);
	for (i = 0; i < n; i++) {
		const gchar *k;

		k = yaml_mapping_get_key(m, i);
		if (key_is_forbidden(k)) {
			g_set_error(error, GOWL_INPUT_REMAP_ERROR,
			            GOWL_INPUT_REMAP_ERROR_NOT_ONE_TO_ONE,
			            "`%s': remaps are one press to one output -- "
			            "no sequences, macros, delays or repeats", k);
			return NULL;
		}
		if (g_strcmp0(k, "name") != 0 && g_strcmp0(k, "match") != 0
		    && g_strcmp0(k, "map") != 0 && g_strcmp0(k, "unmatched") != 0
		    && g_strcmp0(k, "log") != 0) {
			g_set_error(error, GOWL_INPUT_REMAP_ERROR,
			            GOWL_INPUT_REMAP_ERROR_INVALID,
			            "unknown rule key `%s' (name, match, map, "
			            "unmatched, log)", k);
			return NULL;
		}
	}

	if (!require_scalar(yaml_mapping_get_member(m, "name"), "name", &name,
	                    error))
		return NULL;
	rule = gowl_input_remap_rule_new(name);

	match = yaml_mapping_get_mapping_member(m, "match");
	if (match == NULL) {
		g_set_error(error, GOWL_INPUT_REMAP_ERROR,
		            GOWL_INPUT_REMAP_ERROR_INVALID,
		            "rule `%s' has no `match:' block", name);
		return NULL;
	}
	if (!parse_match(rule, match, error))
		return NULL;

	if (yaml_mapping_has_member(m, "unmatched")) {
		if (!require_scalar(yaml_mapping_get_member(m, "unmatched"),
		                    "unmatched", &v, error))
			return NULL;
		if (g_ascii_strcasecmp(v, "drop") == 0)
			rule->drop_unmatched = TRUE;
		else if (g_ascii_strcasecmp(v, "pass") == 0)
			rule->drop_unmatched = FALSE;
		else {
			g_set_error(error, GOWL_INPUT_REMAP_ERROR,
			            GOWL_INPUT_REMAP_ERROR_INVALID,
			            "`unmatched: %s' -- pass or drop", v);
			return NULL;
		}
	}
	if (yaml_mapping_has_member(m, "log")) {
		if (!require_scalar(yaml_mapping_get_member(m, "log"), "log", &v,
		                    error))
			return NULL;
		rule->log = g_ascii_strcasecmp(v, "true") == 0
		            || g_ascii_strcasecmp(v, "yes") == 0
		            || g_strcmp0(v, "1") == 0;
	}

	/* map: may be empty -- a rule that only claims a device and drops
	 * or passes everything is legitimate (unmatched: drop mutes one) */
	if (yaml_mapping_has_member(m, "map")) {
		YamlNode *map_node;

		map_node = yaml_mapping_get_member(m, "map");
		if (map_node != NULL
		    && yaml_node_get_node_type(map_node) == YAML_NODE_SEQUENCE) {
			g_set_error(error, GOWL_INPUT_REMAP_ERROR,
			            GOWL_INPUT_REMAP_ERROR_NOT_ONE_TO_ONE,
			            "rule `%s': `map:' is a list; it is a mapping "
			            "of one input to one output", name);
			return NULL;
		}
		map = yaml_mapping_get_mapping_member(m, "map");
		if (map != NULL) {
			n = yaml_mapping_get_size(map);
			for (i = 0; i < n; i++) {
				if (!parse_map_entry(rule, yaml_mapping_get_key(map, i),
				                     yaml_mapping_get_value(map, i),
				                     error))
					return NULL;
			}
		}
	}

	return (GowlInputRemapRule *)g_steal_pointer(&rule);
}

/* Parses @yaml into its root node, which the parser keeps alive. */
static YamlNode *
load_root(
	YamlParser   *parser,
	const gchar  *yaml,
	GError      **error
){
	YamlNode *root;

	if (yaml == NULL || *yaml == '\0') {
		g_set_error(error, GOWL_INPUT_REMAP_ERROR,
		            GOWL_INPUT_REMAP_ERROR_INVALID, "no rule text");
		return NULL;
	}
	if (!yaml_parser_load_from_data(parser, yaml, -1, error))
		return NULL;
	root = yaml_parser_get_root(parser);
	if (root == NULL) {
		g_set_error(error, GOWL_INPUT_REMAP_ERROR,
		            GOWL_INPUT_REMAP_ERROR_INVALID, "no rule text");
		return NULL;
	}
	return root;
}

/**
 * gowl_input_remap_rule_new_from_yaml:
 * @yaml: one rule as YAML text, block or flow style
 * @error: return location for a #GError
 *
 * What `inputremap-add' reads.  The schema is the one an entry of the
 * config's `input-remap:' list uses:
 *
 * |[
 * {name: pedals, match: {id: "1a86:e026"},
 *  map: {KEY_A: {button: middle}, KEY_B: {action: focus-client, arg: "app-id:*wow*"}}}
 * ]|
 *
 * Returns: (transfer full) (nullable): the rule, or %NULL with @error set
 */
GowlInputRemapRule *
gowl_input_remap_rule_new_from_yaml(
	const gchar  *yaml,
	GError      **error
){
	g_autoptr(YamlParser) parser = NULL;
	YamlNode *root;

	parser = yaml_parser_new();
	root = load_root(parser, yaml, error);
	if (root == NULL)
		return NULL;
	return gowl_input_remap_rule_new_from_node(root, error);
}

/**
 * gowl_input_remap_rules_from_yaml:
 * @yaml: a YAML list of rules, or a single rule
 * @error: return location for a #GError
 *
 * All or nothing: one bad rule fails the lot, so a typo cannot leave
 * half a configuration in force.
 *
 * Returns: (transfer full) (element-type GowlInputRemapRule) (nullable):
 *   the rules, or %NULL with @error set
 */
GPtrArray *
gowl_input_remap_rules_from_yaml(
	const gchar  *yaml,
	GError      **error
){
	g_autoptr(YamlParser) parser = NULL;
	g_autoptr(GPtrArray) rules = NULL;
	YamlNode *root;
	YamlSequence *seq;
	guint i;
	guint n;

	parser = yaml_parser_new();
	root = load_root(parser, yaml, error);
	if (root == NULL)
		return NULL;

	rules = g_ptr_array_new_with_free_func(
		(GDestroyNotify)gowl_input_remap_rule_unref);
	if (yaml_node_get_node_type(root) != YAML_NODE_SEQUENCE) {
		GowlInputRemapRule *r;

		r = gowl_input_remap_rule_new_from_node(root, error);
		if (r == NULL)
			return NULL;
		g_ptr_array_add(rules, r);
		return (GPtrArray *)g_steal_pointer(&rules);
	}

	seq = yaml_node_get_sequence(root);
	n = seq != NULL ? yaml_sequence_get_length(seq) : 0;
	for (i = 0; i < n; i++) {
		GowlInputRemapRule *r;

		r = gowl_input_remap_rule_new_from_node(
			yaml_sequence_get_element(seq, i), error);
		if (r == NULL)
			return NULL;
		g_ptr_array_add(rules, r);
	}
	return (GPtrArray *)g_steal_pointer(&rules);
}

/**
 * gowl_input_remap_callback_generation: (skip)
 *
 * Bumped every time a callback target is mapped.  The config compiler
 * compares it across gowl_config_init(): a compiled C config that
 * mapped a callback must stay loaded, or the function pointer outlives
 * its code.
 *
 * Returns: the current generation
 */
guint
gowl_input_remap_callback_generation(void)
{
	return (guint)g_atomic_int_get((gint *)&callback_generation);
}
