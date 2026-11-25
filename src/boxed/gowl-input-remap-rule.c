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

#include <math.h>
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

/*
 * Absolute axes a game controller reports.  Spelled out rather than
 * generated: gowl-evdev-names.inc carries KEY_ and BTN_ only, and the
 * list of axes a pad can have is short and stable.
 */
static const GowlEvdevName abs_names[] = {
	{ "ABS_X",        ABS_X        },
	{ "ABS_Y",        ABS_Y        },
	{ "ABS_Z",        ABS_Z        },
	{ "ABS_RX",       ABS_RX       },
	{ "ABS_RY",       ABS_RY       },
	{ "ABS_RZ",       ABS_RZ       },
	{ "ABS_THROTTLE", ABS_THROTTLE },
	{ "ABS_RUDDER",   ABS_RUDDER   },
	{ "ABS_WHEEL",    ABS_WHEEL    },
	{ "ABS_GAS",      ABS_GAS      },
	{ "ABS_BRAKE",    ABS_BRAKE    },
	{ "ABS_HAT0X",    ABS_HAT0X    },
	{ "ABS_HAT0Y",    ABS_HAT0Y    },
	{ "ABS_HAT1X",    ABS_HAT1X    },
	{ "ABS_HAT1Y",    ABS_HAT1Y    },
	{ "ABS_HAT2X",    ABS_HAT2X    },
	{ "ABS_HAT2Y",    ABS_HAT2Y    },
	{ "ABS_HAT3X",    ABS_HAT3X    },
	{ "ABS_HAT3Y",    ABS_HAT3Y    },
	{ "ABS_MISC",     ABS_MISC     },
	{ NULL, 0 }
};

/*
 * Friendly names for game-controller inputs.  Button names are
 * POSITIONAL, as the kernel's gamepad documentation defines them
 * (BTN_SOUTH is the bottom face button whatever its label says), and
 * prefixed `pad-' so `a' and `x' stay free for keyboard keys.  A bare
 * stick or trigger name is the whole axis; given a digital target it
 * means the positive direction, like a bare ABS_ name.
 */
static const GowlEvdevName pad_aliases[] = {
	{ "left-stick-left",    GOWL_INPUT_REMAP_ABS_INPUT(ABS_X,     GOWL_INPUT_REMAP_ABS_NEG)   },
	{ "left-stick-right",   GOWL_INPUT_REMAP_ABS_INPUT(ABS_X,     GOWL_INPUT_REMAP_ABS_POS)   },
	{ "left-stick-up",      GOWL_INPUT_REMAP_ABS_INPUT(ABS_Y,     GOWL_INPUT_REMAP_ABS_NEG)   },
	{ "left-stick-down",    GOWL_INPUT_REMAP_ABS_INPUT(ABS_Y,     GOWL_INPUT_REMAP_ABS_POS)   },
	{ "right-stick-left",   GOWL_INPUT_REMAP_ABS_INPUT(ABS_RX,    GOWL_INPUT_REMAP_ABS_NEG)   },
	{ "right-stick-right",  GOWL_INPUT_REMAP_ABS_INPUT(ABS_RX,    GOWL_INPUT_REMAP_ABS_POS)   },
	{ "right-stick-up",     GOWL_INPUT_REMAP_ABS_INPUT(ABS_RY,    GOWL_INPUT_REMAP_ABS_NEG)   },
	{ "right-stick-down",   GOWL_INPUT_REMAP_ABS_INPUT(ABS_RY,    GOWL_INPUT_REMAP_ABS_POS)   },
	{ "left-stick-x",       GOWL_INPUT_REMAP_ABS_INPUT(ABS_X,     GOWL_INPUT_REMAP_ABS_WHOLE) },
	{ "left-stick-y",       GOWL_INPUT_REMAP_ABS_INPUT(ABS_Y,     GOWL_INPUT_REMAP_ABS_WHOLE) },
	{ "right-stick-x",      GOWL_INPUT_REMAP_ABS_INPUT(ABS_RX,    GOWL_INPUT_REMAP_ABS_WHOLE) },
	{ "right-stick-y",      GOWL_INPUT_REMAP_ABS_INPUT(ABS_RY,    GOWL_INPUT_REMAP_ABS_WHOLE) },
	{ "left-trigger",       GOWL_INPUT_REMAP_ABS_INPUT(ABS_Z,     GOWL_INPUT_REMAP_ABS_WHOLE) },
	{ "right-trigger",      GOWL_INPUT_REMAP_ABS_INPUT(ABS_RZ,    GOWL_INPUT_REMAP_ABS_WHOLE) },
	{ "dpad-up",            GOWL_INPUT_REMAP_ABS_INPUT(ABS_HAT0Y, GOWL_INPUT_REMAP_ABS_NEG)   },
	{ "dpad-down",          GOWL_INPUT_REMAP_ABS_INPUT(ABS_HAT0Y, GOWL_INPUT_REMAP_ABS_POS)   },
	{ "dpad-left",          GOWL_INPUT_REMAP_ABS_INPUT(ABS_HAT0X, GOWL_INPUT_REMAP_ABS_NEG)   },
	{ "dpad-right",         GOWL_INPUT_REMAP_ABS_INPUT(ABS_HAT0X, GOWL_INPUT_REMAP_ABS_POS)   },
	{ "pad-a",              BTN_SOUTH  },
	{ "pad-south",          BTN_SOUTH  },
	{ "pad-b",              BTN_EAST   },
	{ "pad-east",           BTN_EAST   },
	{ "pad-x",              BTN_WEST   },
	{ "pad-west",           BTN_WEST   },
	{ "pad-y",              BTN_NORTH  },
	{ "pad-north",          BTN_NORTH  },
	{ "left-bumper",        BTN_TL     },
	{ "right-bumper",       BTN_TR     },
	{ "left-trigger-button",  BTN_TL2  },
	{ "right-trigger-button", BTN_TR2  },
	{ "pad-select",         BTN_SELECT },
	{ "pad-start",          BTN_START  },
	{ "pad-guide",          BTN_MODE   },
	{ "left-stick-press",   BTN_THUMBL },
	{ "right-stick-press",  BTN_THUMBR },
	{ "mouse-x",            GOWL_INPUT_REMAP_REL_X },
	{ "mouse-y",            GOWL_INPUT_REMAP_REL_Y },
	{ NULL, 0 }
};

/* The buttons and axes a virtual pad exposes when its declaration lists
 * none: an Xbox-style controller, the layout games expect. */
static const guint32 default_pad_buttons[] = {
	BTN_SOUTH, BTN_EAST, BTN_NORTH, BTN_WEST, BTN_TL, BTN_TR, BTN_TL2,
	BTN_TR2, BTN_SELECT, BTN_START, BTN_MODE, BTN_THUMBL, BTN_THUMBR
};
static const guint32 default_pad_axes[] = {
	ABS_X, ABS_Y, ABS_Z, ABS_RX, ABS_RY, ABS_RZ, ABS_HAT0X, ABS_HAT0Y
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
	gchar                   *arg;        /* action arg, command, macro name */
	gchar                   *macro_args; /* macro target's arguments */
	GowlInputRemapCallback   callback;
	gpointer                 user_data;
	GDestroyNotify           destroy;
	/* stick / trigger / pointer / scroll targets */
	guint                    side;       /* GOWL_INPUT_REMAP_SIDE_* */
	guint                    component;  /* GOWL_INPUT_REMAP_COMPONENT_* */
	gdouble                  value;      /* deflection while held */
	gboolean                 follow;     /* stick follows pointer motion */
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
	/* game controllers: grab the evdev node, and how axes are read */
	gboolean                  grab;
	GowlInputRemapAnalog      analog;
	/* the virtual pad this rule's stick/trigger/pad-button targets
	 * drive; NULL when it declares none */
	gchar                    *pad_name;
	guint16                   pad_vendor;
	guint16                   pad_product;
	GArray                   *pad_buttons;   /* guint32 BTN_* */
	GArray                   *pad_axes;      /* guint32 ABS_* */
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
	g_free(t->macro_args);
	g_free(t);
}

/* A valid rule input: a real evdev key/button code, a wheel notch, an
 * absolute axis (whole or one direction) or pointer motion.  0 is
 * KEY_RESERVED, never emitted by hardware. */
static gboolean
input_is_valid(guint32 input)
{
	return (input > 0 && input <= KEY_MAX)
	       || (input >= GOWL_BUTTON_WHEEL_UP
	           && input <= GOWL_BUTTON_WHEEL_RIGHT)
	       || GOWL_INPUT_REMAP_INPUT_IS_ABS(input)
	       || GOWL_INPUT_REMAP_INPUT_IS_REL(input);
}

/* Whether @kind is driven continuously rather than by one press. */
static gboolean
kind_is_continuous_input(GowlInputRemapTargetKind kind)
{
	return kind == GOWL_INPUT_REMAP_TARGET_POINTER
	       || kind == GOWL_INPUT_REMAP_TARGET_SCROLL;
}

/* A whole axis given a digital target means its positive direction: a
 * trigger pulled, a stick pushed right or down. */
static guint32
normalise_input(
	guint32                  input,
	GowlInputRemapTargetKind kind
){
	if (GOWL_INPUT_REMAP_INPUT_IS_ABS(input)
	    && GOWL_INPUT_REMAP_ABS_DIR(input) == GOWL_INPUT_REMAP_ABS_WHOLE
	    && !kind_is_continuous_input(kind))
		return GOWL_INPUT_REMAP_ABS_INPUT(GOWL_INPUT_REMAP_ABS_OF(input),
		                                  GOWL_INPUT_REMAP_ABS_POS);
	return input;
}

/* A digital target (one press, one output) cannot take pointer motion:
 * motion has no press.  Only a stick-follow target, drop or pass can. */
static gboolean
check_digital_input(
	guint32   input,
	GError  **error
){
	if (!GOWL_INPUT_REMAP_INPUT_IS_REL(input))
		return TRUE;
	g_set_error(error, GOWL_INPUT_REMAP_ERROR,
	            GOWL_INPUT_REMAP_ERROR_INVALID,
	            "pointer motion (REL_X/REL_Y) has no press; it can only "
	            "drive a virtual stick ({stick: left|right})");
	return FALSE;
}

/* Stick, trigger and pad-button targets drive the rule's virtual pad,
 * which must be declared first. */
static gboolean
check_has_pad(
	GowlInputRemapRule  *self,
	GError             **error
){
	if (self->pad_name != NULL)
		return TRUE;
	g_set_error(error, GOWL_INPUT_REMAP_ERROR,
	            GOWL_INPUT_REMAP_ERROR_INVALID,
	            "rule `%s' maps to a virtual pad but declares none "
	            "(`virtual-pad: {name: NAME}')", self->name);
	return FALSE;
}

/* Whether the rule's virtual pad exposes ABS code @abs. */
static gboolean
pad_has_axis(
	GowlInputRemapRule *self,
	guint32             abs
){
	guint i;

	if (self->pad_axes == NULL)
		return TRUE;
	for (i = 0; i < self->pad_axes->len; i++)
		if (g_array_index(self->pad_axes, guint32, i) == abs)
			return TRUE;
	return FALSE;
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
	input = normalise_input(input, target->kind);
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
	gowl_input_remap_analog_init(&self->analog);
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
	g_free(self->pad_name);
	g_clear_pointer(&self->pad_buttons, g_array_unref);
	g_clear_pointer(&self->pad_axes, g_array_unref);
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

	if (!check_input(input, error) || !check_digital_input(input, error))
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

	if (!check_input(input, error) || !check_digital_input(input, error))
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

	if (!check_input(input, error) || !check_digital_input(input, error))
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

	if (!check_input(input, error) || !check_digital_input(input, error))
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
 * gowl_input_remap_rule_map_macro:
 * @self: a #GowlInputRemapRule
 * @input: the physical input
 * @name: the macro to run on press (a name on the macro path, or a path)
 * @args: (nullable): its arguments, shell-style (`"a b" c')
 * @error: return location for a #GError
 *
 * Maps @input to a macro, run through the opt-in macro module on press.
 * A macro can do many things -- this is OUTSIDE the one-to-one
 * guarantee, like a callback; do not use it where the rules of a game
 * forbid automation.
 *
 * Returns: %TRUE on success
 */
gboolean
gowl_input_remap_rule_map_macro(
	GowlInputRemapRule  *self,
	guint32              input,
	const gchar         *name,
	const gchar         *args,
	GError             **error
){
	GowlInputRemapTarget *t;

	g_return_val_if_fail(self != NULL, FALSE);

	if (!check_input(input, error) || !check_digital_input(input, error))
		return FALSE;
	if (name == NULL || *name == '\0' || strpbrk(name, "\n\r") != NULL
	    || (args != NULL && strpbrk(args, "\n\r") != NULL)) {
		g_set_error(error, GOWL_INPUT_REMAP_ERROR,
		            GOWL_INPUT_REMAP_ERROR_INVALID,
		            "a macro target needs a name, on one line");
		return FALSE;
	}

	t = g_new0(GowlInputRemapTarget, 1);
	t->kind = GOWL_INPUT_REMAP_TARGET_MACRO;
	t->arg = g_strdup(name);
	t->macro_args = (args != NULL && *args != '\0') ? g_strdup(args) : NULL;
	store_target(self, input, t);
	return TRUE;
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

/* --- Game controllers and virtual pads --- */

/**
 * gowl_input_remap_analog_init:
 * @analog: (out caller-allocates): the settings to fill
 *
 * Fills @analog with the defaults every rule starts with: deadzone from
 * the device, press at 0.5, release at 0.35, nothing inverted, 1200
 * px/s pointer speed on a square curve without acceleration, 8 scroll
 * notches per second, 0.02 stick per pixel of mouse motion and a 100 ms
 * return to centre.  Slant and gate are 0: the stick is not corrected.
 */
void
gowl_input_remap_analog_init(GowlInputRemapAnalog *analog)
{
	g_return_if_fail(analog != NULL);

	memset(analog, 0, sizeof *analog);
	analog->deadzone = -1.0;
	analog->press = 0.5;
	analog->release = 0.35;
	analog->pointer_speed = 1200.0;
	analog->pointer_curve = 2.0;
	analog->pointer_accel = 0.0;
	analog->scroll_rate = 8.0;
	analog->mouse_gain = 0.02;
	analog->mouse_idle_ms = 100;
}

/**
 * gowl_input_remap_rule_set_grab:
 * @self: a #GowlInputRemapRule
 * @grab: %TRUE to grab a matched game controller (EVIOCGRAB) so that
 *   nothing else -- the game included -- reads it while the rule holds
 *   it
 *
 * Only game controllers are grabbed; keyboards and pointers are claimed
 * through the compositor as before.  Off by default: merely matching a
 * controller must never take it away from a game.
 */
void
gowl_input_remap_rule_set_grab(
	GowlInputRemapRule *self,
	gboolean            grab
){
	g_return_if_fail(self != NULL);
	self->grab = grab ? TRUE : FALSE;
}

/**
 * gowl_input_remap_rule_get_grab:
 * @self: a #GowlInputRemapRule
 *
 * Returns: whether a matched game controller is grabbed
 */
gboolean
gowl_input_remap_rule_get_grab(GowlInputRemapRule *self)
{
	g_return_val_if_fail(self != NULL, FALSE);
	return self->grab;
}

/**
 * gowl_input_remap_rule_get_analog:
 * @self: a #GowlInputRemapRule
 * @out: (out caller-allocates): the rule's analog settings
 */
void
gowl_input_remap_rule_get_analog(
	GowlInputRemapRule   *self,
	GowlInputRemapAnalog *out
){
	g_return_if_fail(self != NULL);
	g_return_if_fail(out != NULL);
	*out = self->analog;
}

/**
 * gowl_input_remap_rule_set_analog:
 * @self: a #GowlInputRemapRule
 * @analog: the settings; start from gowl_input_remap_analog_init()
 * @error: return location for a #GError
 *
 * Validates and stores @analog.  Refused: a deadzone above 0.95, a press
 * threshold outside (0, 1], a release at or above the press threshold
 * (the gap is what stops a resting stick chattering), a curve outside
 * [0.5, 5], a pointer speed outside (0, 20000], a negative
 * acceleration, a scroll rate outside (0, 120], a mouse gain outside
 * (0, 1], a return to centre outside [10, 5000] ms, a slant outside
 * [-60, 60] degrees, and a gate outside [0, 1].
 *
 * Returns: %TRUE when stored
 */
gboolean
gowl_input_remap_rule_set_analog(
	GowlInputRemapRule         *self,
	const GowlInputRemapAnalog *analog,
	GError                    **error
){
	const gchar *what;

	g_return_val_if_fail(self != NULL, FALSE);
	g_return_val_if_fail(analog != NULL, FALSE);

	what = NULL;
	if (analog->deadzone > 0.95 || isnan(analog->deadzone))
		what = "deadzone must be at most 0.95 (or negative: from the device)";
	else if (!(analog->press > 0.0 && analog->press <= 1.0))
		what = "press must be in (0, 1]";
	else if (!(analog->release >= 0.0 && analog->release < analog->press))
		what = "release must be in [0, press): the gap stops a resting "
		       "stick chattering";
	else if (!(analog->pointer_curve >= 0.5 && analog->pointer_curve <= 5.0))
		what = "pointer-curve must be in [0.5, 5]";
	else if (!(analog->pointer_speed > 0.0 && analog->pointer_speed <= 20000.0))
		what = "pointer-speed must be in (0, 20000] pixels per second";
	else if (!(analog->pointer_accel >= 0.0 && analog->pointer_accel <= 10.0))
		what = "pointer-accel must be in [0, 10]";
	else if (!(analog->scroll_rate > 0.0 && analog->scroll_rate <= 120.0))
		what = "scroll-rate must be in (0, 120] notches per second";
	else if (!(analog->mouse_gain > 0.0 && analog->mouse_gain <= 1.0))
		what = "mouse-gain must be in (0, 1]";
	else if (analog->mouse_idle_ms < 10 || analog->mouse_idle_ms > 5000)
		what = "mouse-idle-ms must be in [10, 5000]";
	else if (!(analog->slant >= -60.0 && analog->slant <= 60.0)
	         || isnan(analog->slant))
		what = "slant must be in [-60, 60] degrees";
	else if (!(analog->gate >= 0.0 && analog->gate <= 1.0)
	         || isnan(analog->gate))
		what = "gate must be in [0, 1]";
	if (what != NULL) {
		g_set_error(error, GOWL_INPUT_REMAP_ERROR,
		            GOWL_INPUT_REMAP_ERROR_INVALID, "rule `%s': analog %s",
		            self->name, what);
		return FALSE;
	}
	self->analog = *analog;
	return TRUE;
}

/**
 * gowl_input_remap_rule_set_virtual_pad:
 * @self: a #GowlInputRemapRule
 * @name: the pad's name; the device is called "gowl virtual pad: NAME".
 *   Rules naming the same pad share one device.
 * @vendor: its USB vendor id, or 0 for gowl's (%GOWL_INPUT_REMAP_PAD_VENDOR)
 * @product: its product id, or 0 for gowl's
 * @error: return location for a #GError
 *
 * Declares the virtual gamepad this rule's stick, trigger and
 * pad-button targets drive.  It is created through /dev/uinput on its
 * first use, not here, and destroyed when no rule declares it any more.
 *
 * Returns: %TRUE on success
 */
gboolean
gowl_input_remap_rule_set_virtual_pad(
	GowlInputRemapRule  *self,
	const gchar         *name,
	guint16              vendor,
	guint16              product,
	GError             **error
){
	const gchar *p;

	g_return_val_if_fail(self != NULL, FALSE);

	if (name == NULL || *name == '\0' || strlen(name) > 48) {
		g_set_error(error, GOWL_INPUT_REMAP_ERROR,
		            GOWL_INPUT_REMAP_ERROR_INVALID,
		            "a virtual pad needs a name of 1..48 characters");
		return FALSE;
	}
	for (p = name; *p != '\0'; p++) {
		if (!g_ascii_isalnum(*p) && strchr("-_. ", *p) == NULL) {
			g_set_error(error, GOWL_INPUT_REMAP_ERROR,
			            GOWL_INPUT_REMAP_ERROR_INVALID,
			            "virtual pad name `%s': letters, digits, `-', "
			            "`_', `.' and spaces only", name);
			return FALSE;
		}
	}
	g_free(self->pad_name);
	self->pad_name = g_strdup(name);
	self->pad_vendor = vendor != 0 ? vendor : GOWL_INPUT_REMAP_PAD_VENDOR;
	self->pad_product = product != 0 ? product : GOWL_INPUT_REMAP_PAD_PRODUCT;
	return TRUE;
}

/**
 * gowl_input_remap_rule_get_virtual_pad:
 * @self: a #GowlInputRemapRule
 *
 * Returns: (transfer none) (nullable): the declared virtual pad's name
 */
const gchar *
gowl_input_remap_rule_get_virtual_pad(GowlInputRemapRule *self)
{
	g_return_val_if_fail(self != NULL, NULL);
	return self->pad_name;
}

/**
 * gowl_input_remap_rule_get_virtual_pad_vendor:
 * @self: a #GowlInputRemapRule
 *
 * Returns: the virtual pad's vendor id (0 without a pad)
 */
guint16
gowl_input_remap_rule_get_virtual_pad_vendor(GowlInputRemapRule *self)
{
	g_return_val_if_fail(self != NULL, 0);
	return self->pad_vendor;
}

/**
 * gowl_input_remap_rule_get_virtual_pad_product:
 * @self: a #GowlInputRemapRule
 *
 * Returns: the virtual pad's product id (0 without a pad)
 */
guint16
gowl_input_remap_rule_get_virtual_pad_product(GowlInputRemapRule *self)
{
	g_return_val_if_fail(self != NULL, 0);
	return self->pad_product;
}

/* A list of codes into a fresh array, each checked by @ok. */
static GArray *
codes_to_array(
	const guint32  *codes,
	guint           n,
	gboolean      (*ok)(guint32 code),
	const gchar    *what,
	GError        **error
){
	GArray *out;
	guint i;

	if (n == 0 || n > 64) {
		g_set_error(error, GOWL_INPUT_REMAP_ERROR,
		            GOWL_INPUT_REMAP_ERROR_INVALID,
		            "a virtual pad lists 1..64 %s", what);
		return NULL;
	}
	out = g_array_sized_new(FALSE, FALSE, sizeof(guint32), n);
	for (i = 0; i < n; i++) {
		if (!ok(codes[i])) {
			g_set_error(error, GOWL_INPUT_REMAP_ERROR,
			            GOWL_INPUT_REMAP_ERROR_UNKNOWN_CODE,
			            "0x%x is not one of the %s a virtual pad can have",
			            codes[i], what);
			g_array_unref(out);
			return NULL;
		}
		g_array_append_val(out, codes[i]);
	}
	return out;
}

static gboolean
pad_button_ok(guint32 code)
{
	/* the joystick and gamepad ranges, and the d-pad buttons */
	return (code >= BTN_JOYSTICK && code < BTN_DIGI)
	       || (code >= BTN_DPAD_UP && code <= BTN_DPAD_RIGHT)
	       || (code >= BTN_TRIGGER_HAPPY && code <= BTN_TRIGGER_HAPPY40);
}

static gboolean
pad_axis_ok(guint32 code)
{
	return code <= ABS_BRAKE || (code >= ABS_HAT0X && code <= ABS_HAT3Y);
}

/**
 * gowl_input_remap_rule_set_virtual_pad_buttons:
 * @self: a #GowlInputRemapRule with a virtual pad
 * @buttons: (array length=n_buttons): BTN_* codes
 * @n_buttons: how many
 * @error: return location for a #GError
 *
 * The buttons the virtual pad exposes, instead of the default Xbox-style
 * set.
 *
 * Returns: %TRUE on success
 */
gboolean
gowl_input_remap_rule_set_virtual_pad_buttons(
	GowlInputRemapRule  *self,
	const guint32       *buttons,
	guint                n_buttons,
	GError             **error
){
	GArray *a;

	g_return_val_if_fail(self != NULL, FALSE);
	if (!check_has_pad(self, error))
		return FALSE;
	a = codes_to_array(buttons, n_buttons, pad_button_ok, "buttons", error);
	if (a == NULL)
		return FALSE;
	g_clear_pointer(&self->pad_buttons, g_array_unref);
	self->pad_buttons = a;
	return TRUE;
}

/**
 * gowl_input_remap_rule_get_virtual_pad_buttons:
 * @self: a #GowlInputRemapRule
 *
 * Returns: (transfer full) (element-type guint32): the buttons the
 *   virtual pad exposes (the default set unless the rule lists its own)
 */
GArray *
gowl_input_remap_rule_get_virtual_pad_buttons(GowlInputRemapRule *self)
{
	GArray *out;

	g_return_val_if_fail(self != NULL, NULL);
	if (self->pad_buttons != NULL)
		return g_array_ref(self->pad_buttons);
	out = g_array_new(FALSE, FALSE, sizeof(guint32));
	g_array_append_vals(out, default_pad_buttons,
	                    G_N_ELEMENTS(default_pad_buttons));
	return out;
}

/**
 * gowl_input_remap_rule_set_virtual_pad_axes:
 * @self: a #GowlInputRemapRule with a virtual pad
 * @axes: (array length=n_axes): ABS_* codes
 * @n_axes: how many
 * @error: return location for a #GError
 *
 * The axes the virtual pad exposes, instead of the default two sticks,
 * two triggers and the d-pad hat.
 *
 * Returns: %TRUE on success
 */
gboolean
gowl_input_remap_rule_set_virtual_pad_axes(
	GowlInputRemapRule  *self,
	const guint32       *axes,
	guint                n_axes,
	GError             **error
){
	GArray *a;

	g_return_val_if_fail(self != NULL, FALSE);
	if (!check_has_pad(self, error))
		return FALSE;
	a = codes_to_array(axes, n_axes, pad_axis_ok, "axes", error);
	if (a == NULL)
		return FALSE;
	g_clear_pointer(&self->pad_axes, g_array_unref);
	self->pad_axes = a;
	return TRUE;
}

/**
 * gowl_input_remap_rule_get_virtual_pad_axes:
 * @self: a #GowlInputRemapRule
 *
 * Returns: (transfer full) (element-type guint32): the axes the virtual
 *   pad exposes
 */
GArray *
gowl_input_remap_rule_get_virtual_pad_axes(GowlInputRemapRule *self)
{
	GArray *out;

	g_return_val_if_fail(self != NULL, NULL);
	if (self->pad_axes != NULL)
		return g_array_ref(self->pad_axes);
	out = g_array_new(FALSE, FALSE, sizeof(guint32));
	g_array_append_vals(out, default_pad_axes, G_N_ELEMENTS(default_pad_axes));
	return out;
}

/**
 * gowl_input_remap_rule_has_continuous:
 * @self: a #GowlInputRemapRule
 *
 * Returns: whether any target of @self is continuous (pointer, scroll,
 *   or a stick following the mouse) -- the declared exceptions to the
 *   one-to-one rule, which `inputremap-list' reports
 */
gboolean
gowl_input_remap_rule_has_continuous(GowlInputRemapRule *self)
{
	GHashTableIter iter;
	gpointer value;

	g_return_val_if_fail(self != NULL, FALSE);
	g_hash_table_iter_init(&iter, self->map);
	while (g_hash_table_iter_next(&iter, NULL, &value))
		if (gowl_input_remap_target_is_continuous(value))
			return TRUE;
	return FALSE;
}

/* The virtual-pad ABS code a stick/trigger target drives. */
static guint32
pad_target_axis(
	guint kind_is_trigger,
	guint side,
	guint component
){
	if (kind_is_trigger)
		return side == GOWL_INPUT_REMAP_SIDE_RIGHT ? ABS_RZ : ABS_Z;
	if (side == GOWL_INPUT_REMAP_SIDE_DPAD)
		return component == GOWL_INPUT_REMAP_COMPONENT_Y ? ABS_HAT0Y : ABS_HAT0X;
	if (side == GOWL_INPUT_REMAP_SIDE_RIGHT)
		return component == GOWL_INPUT_REMAP_COMPONENT_Y ? ABS_RY : ABS_RX;
	return component == GOWL_INPUT_REMAP_COMPONENT_Y ? ABS_Y : ABS_X;
}

/**
 * gowl_input_remap_rule_map_pointer:
 * @self: a #GowlInputRemapRule
 * @input: a WHOLE absolute axis (GOWL_INPUT_REMAP_ABS_INPUT(ABS_X,
 *   GOWL_INPUT_REMAP_ABS_WHOLE))
 * @component: %GOWL_INPUT_REMAP_COMPONENT_X or _Y: which way the
 *   pointer moves
 * @error: return location for a #GError
 *
 * The axis moves the pointer for as long as it is out of its deadzone,
 * at the rule's analog speed.  CONTINUOUS: outside the one-to-one rule,
 * by declaration.
 *
 * Returns: %TRUE on success
 */
gboolean
gowl_input_remap_rule_map_pointer(
	GowlInputRemapRule  *self,
	guint32              input,
	guint                component,
	GError             **error
){
	GowlInputRemapTarget *t;

	g_return_val_if_fail(self != NULL, FALSE);
	if (!GOWL_INPUT_REMAP_INPUT_IS_ABS(input)
	    || GOWL_INPUT_REMAP_ABS_DIR(input) != GOWL_INPUT_REMAP_ABS_WHOLE) {
		g_set_error(error, GOWL_INPUT_REMAP_ERROR,
		            GOWL_INPUT_REMAP_ERROR_INVALID,
		            "a pointer target needs a whole analog axis (ABS_X, "
		            "right-stick-x, ...), not a key or a direction");
		return FALSE;
	}
	if (component > GOWL_INPUT_REMAP_COMPONENT_Y) {
		g_set_error(error, GOWL_INPUT_REMAP_ERROR,
		            GOWL_INPUT_REMAP_ERROR_INVALID,
		            "a pointer target is x or y");
		return FALSE;
	}
	t = g_new0(GowlInputRemapTarget, 1);
	t->kind = GOWL_INPUT_REMAP_TARGET_POINTER;
	t->component = component;
	store_target(self, input, t);
	return TRUE;
}

/**
 * gowl_input_remap_rule_map_scroll:
 * @self: a #GowlInputRemapRule
 * @input: a WHOLE absolute axis
 * @horizontal: %TRUE to scroll sideways
 * @error: return location for a #GError
 *
 * The axis scrolls, in whole wheel notches at the rule's scroll rate,
 * for as long as it is out of its deadzone.  Continuous, like
 * gowl_input_remap_rule_map_pointer().
 *
 * Returns: %TRUE on success
 */
gboolean
gowl_input_remap_rule_map_scroll(
	GowlInputRemapRule  *self,
	guint32              input,
	gboolean             horizontal,
	GError             **error
){
	GowlInputRemapTarget *t;

	g_return_val_if_fail(self != NULL, FALSE);
	if (!GOWL_INPUT_REMAP_INPUT_IS_ABS(input)
	    || GOWL_INPUT_REMAP_ABS_DIR(input) != GOWL_INPUT_REMAP_ABS_WHOLE) {
		g_set_error(error, GOWL_INPUT_REMAP_ERROR,
		            GOWL_INPUT_REMAP_ERROR_INVALID,
		            "a scroll target needs a whole analog axis (ABS_RY, "
		            "right-stick-y, ...), not a key or a direction");
		return FALSE;
	}
	t = g_new0(GowlInputRemapTarget, 1);
	t->kind = GOWL_INPUT_REMAP_TARGET_SCROLL;
	t->component = horizontal ? GOWL_INPUT_REMAP_COMPONENT_X
	                          : GOWL_INPUT_REMAP_COMPONENT_Y;
	store_target(self, input, t);
	return TRUE;
}

/**
 * gowl_input_remap_rule_map_stick:
 * @self: a #GowlInputRemapRule with a virtual pad
 * @input: a key, button or stick direction
 * @side: %GOWL_INPUT_REMAP_SIDE_LEFT, _RIGHT or _DPAD
 * @component: %GOWL_INPUT_REMAP_COMPONENT_X or _Y
 * @value: the deflection while held, [-1, 1], not 0 (evdev's +Y is down)
 * @error: return location for a #GError
 *
 * While @input is held, that component of the virtual stick is
 * deflected by @value; on release it returns.  Several held inputs on
 * one stick combine: opposite directions cancel and a diagonal is
 * normalised onto the unit circle.  One press, one held state: inside
 * the one-to-one rule.
 *
 * Returns: %TRUE on success
 */
gboolean
gowl_input_remap_rule_map_stick(
	GowlInputRemapRule  *self,
	guint32              input,
	guint                side,
	guint                component,
	gdouble              value,
	GError             **error
){
	GowlInputRemapTarget *t;

	g_return_val_if_fail(self != NULL, FALSE);
	if (!check_input(input, error) || !check_digital_input(input, error)
	    || !check_has_pad(self, error))
		return FALSE;
	if (side > GOWL_INPUT_REMAP_SIDE_DPAD
	    || component > GOWL_INPUT_REMAP_COMPONENT_Y
	    || !(value >= -1.0 && value <= 1.0) || value == 0.0) {
		g_set_error(error, GOWL_INPUT_REMAP_ERROR,
		            GOWL_INPUT_REMAP_ERROR_INVALID,
		            "a stick target is left, right or dpad, x or y, "
		            "and a deflection in [-1, 1] other than 0");
		return FALSE;
	}
	if (!pad_has_axis(self, pad_target_axis(FALSE, side, component))) {
		g_set_error(error, GOWL_INPUT_REMAP_ERROR,
		            GOWL_INPUT_REMAP_ERROR_INVALID,
		            "virtual pad `%s' has no axis for that stick",
		            self->pad_name);
		return FALSE;
	}
	t = g_new0(GowlInputRemapTarget, 1);
	t->kind = GOWL_INPUT_REMAP_TARGET_STICK;
	t->side = side;
	t->component = component;
	t->value = value;
	store_target(self, input, t);
	return TRUE;
}

/**
 * gowl_input_remap_rule_map_stick_follow:
 * @self: a #GowlInputRemapRule with a virtual pad
 * @input: %GOWL_INPUT_REMAP_REL_X or %GOWL_INPUT_REMAP_REL_Y
 * @side: %GOWL_INPUT_REMAP_SIDE_LEFT or _RIGHT
 * @error: return location for a #GError
 *
 * Mouse-look onto a controller: the claimed pointer's motion along
 * @input deflects that component of the stick (the rule's mouse gain
 * per pixel), which returns to centre once the mouse has been still for
 * the rule's mouse-idle-ms.  The pointer itself does not move.
 * CONTINUOUS: a declared exception to the one-to-one rule.
 *
 * Returns: %TRUE on success
 */
gboolean
gowl_input_remap_rule_map_stick_follow(
	GowlInputRemapRule  *self,
	guint32              input,
	guint                side,
	GError             **error
){
	GowlInputRemapTarget *t;

	g_return_val_if_fail(self != NULL, FALSE);
	if (!GOWL_INPUT_REMAP_INPUT_IS_REL(input)) {
		g_set_error(error, GOWL_INPUT_REMAP_ERROR,
		            GOWL_INPUT_REMAP_ERROR_INVALID,
		            "a stick that follows the mouse takes REL_X or REL_Y "
		            "(mouse-x, mouse-y); a key needs `x:' or `y:'");
		return FALSE;
	}
	if (!check_has_pad(self, error))
		return FALSE;
	if (side > GOWL_INPUT_REMAP_SIDE_RIGHT) {
		g_set_error(error, GOWL_INPUT_REMAP_ERROR,
		            GOWL_INPUT_REMAP_ERROR_INVALID,
		            "the mouse can drive the left or right stick");
		return FALSE;
	}
	t = g_new0(GowlInputRemapTarget, 1);
	t->kind = GOWL_INPUT_REMAP_TARGET_STICK;
	t->side = side;
	t->component = input == GOWL_INPUT_REMAP_REL_Y
	               ? GOWL_INPUT_REMAP_COMPONENT_Y : GOWL_INPUT_REMAP_COMPONENT_X;
	t->follow = TRUE;
	store_target(self, input, t);
	return TRUE;
}

/**
 * gowl_input_remap_rule_map_trigger:
 * @self: a #GowlInputRemapRule with a virtual pad
 * @input: a key, button or stick direction
 * @side: %GOWL_INPUT_REMAP_SIDE_LEFT or _RIGHT
 * @value: how far the trigger is pulled while held, (0, 1]
 * @error: return location for a #GError
 *
 * Returns: %TRUE on success
 */
gboolean
gowl_input_remap_rule_map_trigger(
	GowlInputRemapRule  *self,
	guint32              input,
	guint                side,
	gdouble              value,
	GError             **error
){
	GowlInputRemapTarget *t;

	g_return_val_if_fail(self != NULL, FALSE);
	if (!check_input(input, error) || !check_digital_input(input, error)
	    || !check_has_pad(self, error))
		return FALSE;
	if (side > GOWL_INPUT_REMAP_SIDE_RIGHT
	    || !(value > 0.0 && value <= 1.0)) {
		g_set_error(error, GOWL_INPUT_REMAP_ERROR,
		            GOWL_INPUT_REMAP_ERROR_INVALID,
		            "a trigger target is left or right, pulled (0, 1]");
		return FALSE;
	}
	if (!pad_has_axis(self, pad_target_axis(TRUE, side, 0))) {
		g_set_error(error, GOWL_INPUT_REMAP_ERROR,
		            GOWL_INPUT_REMAP_ERROR_INVALID,
		            "virtual pad `%s' has no axis for that trigger",
		            self->pad_name);
		return FALSE;
	}
	t = g_new0(GowlInputRemapTarget, 1);
	t->kind = GOWL_INPUT_REMAP_TARGET_TRIGGER;
	t->side = side;
	t->value = value;
	store_target(self, input, t);
	return TRUE;
}

/**
 * gowl_input_remap_rule_map_pad_button:
 * @self: a #GowlInputRemapRule with a virtual pad
 * @input: a key, button or stick direction
 * @button: the virtual pad's BTN_* to hold while @input is held
 * @error: return location for a #GError
 *
 * Returns: %TRUE on success
 */
gboolean
gowl_input_remap_rule_map_pad_button(
	GowlInputRemapRule  *self,
	guint32              input,
	guint32              button,
	GError             **error
){
	GowlInputRemapTarget *t;
	g_autoptr(GArray) buttons = NULL;
	gboolean found;
	guint i;

	g_return_val_if_fail(self != NULL, FALSE);
	if (!check_input(input, error) || !check_digital_input(input, error)
	    || !check_has_pad(self, error))
		return FALSE;
	buttons = gowl_input_remap_rule_get_virtual_pad_buttons(self);
	found = FALSE;
	for (i = 0; i < buttons->len; i++)
		if (g_array_index(buttons, guint32, i) == button)
			found = TRUE;
	if (!found) {
		g_autofree gchar *n = gowl_input_remap_code_to_name(button);

		g_set_error(error, GOWL_INPUT_REMAP_ERROR,
		            GOWL_INPUT_REMAP_ERROR_UNKNOWN_CODE,
		            "virtual pad `%s' has no button %s", self->pad_name, n);
		return FALSE;
	}
	t = g_new0(GowlInputRemapTarget, 1);
	t->kind = GOWL_INPUT_REMAP_TARGET_PAD_BUTTON;
	t->code = button;
	store_target(self, input, t);
	return TRUE;
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
 * gowl_input_remap_target_get_macro_args:
 * @self: a #GowlInputRemapTarget
 *
 * Returns: (transfer none) (nullable): a macro target's arguments
 */
const gchar *
gowl_input_remap_target_get_macro_args(const GowlInputRemapTarget *self)
{
	g_return_val_if_fail(self != NULL, NULL);
	return self->macro_args;
}

/**
 * gowl_input_remap_target_get_side:
 * @self: a #GowlInputRemapTarget
 *
 * Returns: a stick or trigger target's %GOWL_INPUT_REMAP_SIDE_*
 */
guint
gowl_input_remap_target_get_side(const GowlInputRemapTarget *self)
{
	g_return_val_if_fail(self != NULL, 0);
	return self->side;
}

/**
 * gowl_input_remap_target_get_component:
 * @self: a #GowlInputRemapTarget
 *
 * Returns: a stick or pointer target's %GOWL_INPUT_REMAP_COMPONENT_*;
 *   for a scroll target, _X when it scrolls horizontally
 */
guint
gowl_input_remap_target_get_component(const GowlInputRemapTarget *self)
{
	g_return_val_if_fail(self != NULL, 0);
	return self->component;
}

/**
 * gowl_input_remap_target_get_value:
 * @self: a #GowlInputRemapTarget
 *
 * Returns: a stick or trigger target's deflection while held
 */
gdouble
gowl_input_remap_target_get_value(const GowlInputRemapTarget *self)
{
	g_return_val_if_fail(self != NULL, 0.0);
	return self->value;
}

/**
 * gowl_input_remap_target_is_follow:
 * @self: a #GowlInputRemapTarget
 *
 * Returns: whether a stick target follows pointer motion
 */
gboolean
gowl_input_remap_target_is_follow(const GowlInputRemapTarget *self)
{
	g_return_val_if_fail(self != NULL, FALSE);
	return self->follow;
}

/**
 * gowl_input_remap_target_is_continuous:
 * @self: a #GowlInputRemapTarget
 *
 * The declared exceptions to the one-to-one rule: pointer and scroll
 * targets, and a stick following the mouse, produce output for as long
 * as their input is deflected rather than once per press.
 *
 * Returns: whether @self is continuous
 */
gboolean
gowl_input_remap_target_is_continuous(const GowlInputRemapTarget *self)
{
	g_return_val_if_fail(self != NULL, FALSE);
	return kind_is_continuous_input(self->kind)
	       || (self->kind == GOWL_INPUT_REMAP_TARGET_STICK && self->follow);
}

/**
 * gowl_input_remap_target_is_pad:
 * @self: a #GowlInputRemapTarget
 *
 * Returns: whether @self drives the rule's virtual pad (stick, trigger
 *   or pad button)
 */
gboolean
gowl_input_remap_target_is_pad(const GowlInputRemapTarget *self)
{
	g_return_val_if_fail(self != NULL, FALSE);
	return self->kind == GOWL_INPUT_REMAP_TARGET_STICK
	       || self->kind == GOWL_INPUT_REMAP_TARGET_TRIGGER
	       || self->kind == GOWL_INPUT_REMAP_TARGET_PAD_BUTTON;
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

/* A number the YAML loader reads back exactly, whatever the locale. */
static void
append_number(
	GString *out,
	gdouble  v
){
	gchar buf[G_ASCII_DTOSTR_BUF_SIZE];

	g_string_append(out, g_ascii_formatd(buf, sizeof buf, "%g", v));
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
	case GOWL_INPUT_REMAP_TARGET_MACRO:
		g_string_append(out, "{macro: ");
		append_quoted(out, t->arg);
		if (t->macro_args != NULL) {
			g_string_append(out, ", args: ");
			append_quoted(out, t->macro_args);
		}
		g_string_append_c(out, '}');
		return;
	case GOWL_INPUT_REMAP_TARGET_POINTER:
		g_string_append_printf(out, "{pointer: %s}",
		                       t->component == GOWL_INPUT_REMAP_COMPONENT_Y
		                       ? "y" : "x");
		return;
	case GOWL_INPUT_REMAP_TARGET_SCROLL:
		g_string_append_printf(out, "{scroll: %s}",
		                       t->component == GOWL_INPUT_REMAP_COMPONENT_X
		                       ? "horizontal" : "vertical");
		return;
	case GOWL_INPUT_REMAP_TARGET_STICK: {
		static const gchar *const sides[] = { "left", "right", "dpad" };

		g_string_append_printf(out, "{stick: %s", sides[MIN(t->side, 2u)]);
		if (!t->follow) {
			g_string_append_printf(out, ", %s: ",
			                       t->component == GOWL_INPUT_REMAP_COMPONENT_Y
			                       ? "y" : "x");
			append_number(out, t->value);
		}
		g_string_append_c(out, '}');
		return;
	}
	case GOWL_INPUT_REMAP_TARGET_TRIGGER:
		g_string_append_printf(out, "{trigger: %s, value: ",
		                       t->side == GOWL_INPUT_REMAP_SIDE_RIGHT
		                       ? "right" : "left");
		append_number(out, t->value);
		g_string_append_c(out, '}');
		return;
	case GOWL_INPUT_REMAP_TARGET_PAD_BUTTON: {
		g_autofree gchar *n = gowl_input_remap_code_to_name(t->code);

		g_string_append_printf(out, "{pad-button: %s}", n);
		return;
	}
	case GOWL_INPUT_REMAP_TARGET_CALLBACK:
	default:
		/* Code has no text form.  Written so a listing shows it;
		 * the loader refuses it, since only C can register one. */
		g_string_append(out, "{callback: native}");
		return;
	}
}

/* One ABS bitmask as a flow list, `[ABS_Y, ABS_RY]'. */
static void
append_abs_mask(
	GString     *out,
	const gchar *key,
	guint64      mask
){
	guint i;
	gboolean first;

	g_string_append_printf(out, ", %s: [", key);
	first = TRUE;
	for (i = 0; i < GOWL_INPUT_REMAP_ABS_COUNT; i++) {
		if ((mask & ((guint64)1 << i)) == 0)
			continue;
		{
			g_autofree gchar *n = gowl_input_remap_code_to_name(
				GOWL_INPUT_REMAP_ABS_INPUT(i, GOWL_INPUT_REMAP_ABS_WHOLE));

			g_string_append_printf(out, "%s%s", first ? "" : ", ", n);
			first = FALSE;
		}
	}
	g_string_append_c(out, ']');
}

/* `, analog: {...}' with only the settings that differ from the
 * defaults, so a rule that never mentions analog lists without it. */
static void
append_analog_yaml(
	GString                    *out,
	const GowlInputRemapAnalog *a
){
	GowlInputRemapAnalog d;
	GString *body;

	gowl_input_remap_analog_init(&d);
	body = g_string_new(NULL);
#define ANALOG_NUM(field, key) \
	if (a->field != d.field) { \
		g_string_append(body, ", " key ": "); \
		append_number(body, (gdouble)a->field); \
	}
	ANALOG_NUM(deadzone, "deadzone")
	ANALOG_NUM(press, "press")
	ANALOG_NUM(release, "release")
	ANALOG_NUM(pointer_speed, "pointer-speed")
	ANALOG_NUM(pointer_curve, "pointer-curve")
	ANALOG_NUM(pointer_accel, "pointer-accel")
	ANALOG_NUM(scroll_rate, "scroll-rate")
	ANALOG_NUM(mouse_gain, "mouse-gain")
	ANALOG_NUM(mouse_idle_ms, "mouse-idle-ms")
	ANALOG_NUM(slant, "slant")
	ANALOG_NUM(gate, "gate")
#undef ANALOG_NUM
	if (a->invert != 0)
		append_abs_mask(body, "invert", a->invert);
	if (a->centred != 0)
		append_abs_mask(body, "centred", a->centred);
	if (body->len > 0)
		g_string_append_printf(out, ", analog: {%s}", body->str + 2);
	g_string_free(body, TRUE);
}

/* `, virtual-pad: {...}' when the rule declares one. */
static void
append_pad_yaml(
	GString            *out,
	GowlInputRemapRule *self
){
	guint i;

	if (self->pad_name == NULL)
		return;
	g_string_append(out, ", virtual-pad: {name: ");
	append_quoted(out, self->pad_name);
	if (self->pad_vendor != GOWL_INPUT_REMAP_PAD_VENDOR
	    || self->pad_product != GOWL_INPUT_REMAP_PAD_PRODUCT)
		g_string_append_printf(out, ", id: \"%04x:%04x\"",
		                       (guint)self->pad_vendor,
		                       (guint)self->pad_product);
	if (self->pad_buttons != NULL) {
		g_string_append(out, ", buttons: [");
		for (i = 0; i < self->pad_buttons->len; i++) {
			g_autofree gchar *n = gowl_input_remap_code_to_name(
				g_array_index(self->pad_buttons, guint32, i));

			g_string_append_printf(out, "%s%s", i == 0 ? "" : ", ", n);
		}
		g_string_append_c(out, ']');
	}
	if (self->pad_axes != NULL) {
		g_string_append(out, ", axes: [");
		for (i = 0; i < self->pad_axes->len; i++) {
			g_autofree gchar *n = gowl_input_remap_code_to_name(
				GOWL_INPUT_REMAP_ABS_INPUT(
					g_array_index(self->pad_axes, guint32, i),
					GOWL_INPUT_REMAP_ABS_WHOLE));

			g_string_append_printf(out, "%s%s", i == 0 ? "" : ", ", n);
		}
		g_string_append_c(out, ']');
	}
	g_string_append_c(out, '}');
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
	if (self->match_type != GOWL_INPUT_REMAP_DEVICE_ANY) {
		GEnumClass *klass;
		GEnumValue *val;

		klass = (GEnumClass *)g_type_class_ref(GOWL_TYPE_INPUT_REMAP_DEVICE_TYPE);
		val = g_enum_get_value(klass, (gint)self->match_type);
		g_string_append_printf(out, "%stype: %s", first ? "" : ", ",
		                       val != NULL ? val->value_nick : "any");
		g_type_class_unref(klass);
	}
	g_string_append_c(out, '}');

	if (self->drop_unmatched)
		g_string_append(out, ", unmatched: drop");
	if (self->log)
		g_string_append(out, ", log: true");
	if (self->grab)
		g_string_append(out, ", grab: true");
	append_analog_yaml(out, &self->analog);
	append_pad_yaml(out, self);

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

	/* Game-controller inputs: ABS_X, ABS_X+, ABS_X-, REL_X, aliases */
	{
		g_autofree gchar *alias = g_ascii_strdown(name, -1);
		gsize len;
		guint dir;

		g_strdelimit(alias, "_", '-');
		if (lookup_table(pad_aliases, alias, FALSE, &code)) {
			*out_code = code;
			return TRUE;
		}
		if (g_strcmp0(norm, "REL_X") == 0) {
			*out_code = GOWL_INPUT_REMAP_REL_X;
			return TRUE;
		}
		if (g_strcmp0(norm, "REL_Y") == 0) {
			*out_code = GOWL_INPUT_REMAP_REL_Y;
			return TRUE;
		}
		/* The sign is read off the name as given: `norm' has had its
		 * `-' turned into `_' for the KEY_/BTN_ lookups above. */
		len = strlen(name);
		dir = GOWL_INPUT_REMAP_ABS_WHOLE;
		if (len > 1 && (name[len - 1] == '+' || name[len - 1] == '-')) {
			dir = name[len - 1] == '+' ? GOWL_INPUT_REMAP_ABS_POS
			                           : GOWL_INPUT_REMAP_ABS_NEG;
			norm[len - 1] = '\0';
		}
		if (lookup_table(abs_names, norm, FALSE, &code)) {
			*out_code = GOWL_INPUT_REMAP_ABS_INPUT(code, dir);
			return TRUE;
		}
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
	if (code == GOWL_INPUT_REMAP_REL_X)
		return g_strdup("REL_X");
	if (code == GOWL_INPUT_REMAP_REL_Y)
		return g_strdup("REL_Y");
	if (GOWL_INPUT_REMAP_INPUT_IS_ABS(code)) {
		guint32 abs;
		guint dir;

		abs = GOWL_INPUT_REMAP_ABS_OF(code);
		dir = GOWL_INPUT_REMAP_ABS_DIR(code);
		for (e = abs_names; e->name != NULL; e++)
			if (e->code == abs)
				return g_strdup_printf("%s%s", e->name,
				                       dir == GOWL_INPUT_REMAP_ABS_POS ? "+"
				                       : dir == GOWL_INPUT_REMAP_ABS_NEG ? "-"
				                       : "");
		return g_strdup_printf("ABS_0x%02x%s", abs,
		                       dir == GOWL_INPUT_REMAP_ABS_POS ? "+"
		                       : dir == GOWL_INPUT_REMAP_ABS_NEG ? "-" : "");
	}
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

/* `left', `right' (and `dpad' for a stick). */
static gboolean
parse_side(
	const gchar  *text,
	gboolean      allow_dpad,
	guint        *out,
	GError      **error
){
	if (g_ascii_strcasecmp(text, "left") == 0)
		*out = GOWL_INPUT_REMAP_SIDE_LEFT;
	else if (g_ascii_strcasecmp(text, "right") == 0)
		*out = GOWL_INPUT_REMAP_SIDE_RIGHT;
	else if (allow_dpad && g_ascii_strcasecmp(text, "dpad") == 0)
		*out = GOWL_INPUT_REMAP_SIDE_DPAD;
	else {
		g_set_error(error, GOWL_INPUT_REMAP_ERROR,
		            GOWL_INPUT_REMAP_ERROR_INVALID,
		            "`%s' -- left or right%s", text,
		            allow_dpad ? " (or dpad)" : "");
		return FALSE;
	}
	return TRUE;
}

/* A number scalar in [@lo, @hi], read in the C locale. */
static gboolean
parse_unit_number(
	YamlNode     *node,
	const gchar  *what,
	gdouble       lo,
	gdouble       hi,
	gdouble      *out,
	GError      **error
){
	const gchar *text;
	gchar *end;
	gdouble v;

	if (!require_scalar(node, what, &text, error))
		return FALSE;
	v = g_ascii_strtod(text, &end);
	if (end == text || *end != '\0' || isnan(v) || v < lo || v > hi) {
		g_set_error(error, GOWL_INPUT_REMAP_ERROR,
		            GOWL_INPUT_REMAP_ERROR_INVALID,
		            "`%s: %s' is not a number in [%g, %g]", what, text, lo, hi);
		return FALSE;
	}
	*out = v;
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
		/* `macro' names a macro TARGET here (outside the one-to-one
		   guarantee, by explicit request); it stays refused as a key of
		   the rule itself, where it would mean a sequence. */
		if (key_is_forbidden(k) && g_strcmp0(k, "macro") != 0) {
			g_set_error(error, GOWL_INPUT_REMAP_ERROR,
			            GOWL_INPUT_REMAP_ERROR_NOT_ONE_TO_ONE,
			            "`%s' in the mapping for %s: remaps are one "
			            "press to one output -- no sequences, macros, "
			            "delays or repeats", k, input_name);
			return FALSE;
		}
		if (g_strcmp0(k, "key") == 0 || g_strcmp0(k, "button") == 0
		    || g_strcmp0(k, "action") == 0
		    || g_strcmp0(k, "command") == 0
		    || g_strcmp0(k, "macro") == 0
		    || g_strcmp0(k, "pointer") == 0
		    || g_strcmp0(k, "scroll") == 0
		    || g_strcmp0(k, "stick") == 0
		    || g_strcmp0(k, "trigger") == 0
		    || g_strcmp0(k, "pad-button") == 0)
			kinds++;
		else if (g_strcmp0(k, "callback") == 0) {
			g_set_error(error, GOWL_INPUT_REMAP_ERROR,
			            GOWL_INPUT_REMAP_ERROR_INVALID,
			            "`callback' for %s: code can only be mapped "
			            "from C (gowl_input_remap_rule_map_callback) "
			            "or, in cmacs, from Elisp", input_name);
			return FALSE;
		} else if (g_strcmp0(k, "arg") != 0 && g_strcmp0(k, "args") != 0
		           && g_strcmp0(k, "x") != 0 && g_strcmp0(k, "y") != 0
		           && g_strcmp0(k, "value") != 0) {
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
		            "of key, button, action, command, macro, pointer, "
		            "scroll, stick, trigger or pad-button", input_name,
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
	if (yaml_mapping_has_member(m, "args")
	    && !yaml_mapping_has_member(m, "macro")) {
		g_set_error(error, GOWL_INPUT_REMAP_ERROR,
		            GOWL_INPUT_REMAP_ERROR_INVALID,
		            "`args' for %s only goes with `macro'", input_name);
		return FALSE;
	}

	/* x/y go with a stick, value with a trigger; and a stick takes one
	 * of x or y (two would be two outputs) */
	if ((yaml_mapping_has_member(m, "x") || yaml_mapping_has_member(m, "y"))
	    && !yaml_mapping_has_member(m, "stick")) {
		g_set_error(error, GOWL_INPUT_REMAP_ERROR,
		            GOWL_INPUT_REMAP_ERROR_INVALID,
		            "`x'/`y' for %s only go with `stick'", input_name);
		return FALSE;
	}
	if (yaml_mapping_has_member(m, "value")
	    && !yaml_mapping_has_member(m, "trigger")) {
		g_set_error(error, GOWL_INPUT_REMAP_ERROR,
		            GOWL_INPUT_REMAP_ERROR_INVALID,
		            "`value' for %s only goes with `trigger'", input_name);
		return FALSE;
	}

	/* Exactly one kind is present: build it */
	if (yaml_mapping_has_member(m, "pointer")
	    || yaml_mapping_has_member(m, "scroll")) {
		gboolean is_pointer;

		is_pointer = yaml_mapping_has_member(m, "pointer");
		if (!require_scalar(yaml_mapping_get_member(m,
		                        is_pointer ? "pointer" : "scroll"),
		                    is_pointer ? "pointer" : "scroll", &text, error))
			return FALSE;
		if (is_pointer) {
			if (g_ascii_strcasecmp(text, "x") != 0
			    && g_ascii_strcasecmp(text, "y") != 0) {
				g_set_error(error, GOWL_INPUT_REMAP_ERROR,
				            GOWL_INPUT_REMAP_ERROR_INVALID,
				            "`pointer: %s' -- x or y", text);
				return FALSE;
			}
			return gowl_input_remap_rule_map_pointer(rule, input,
				g_ascii_strcasecmp(text, "y") == 0
				? GOWL_INPUT_REMAP_COMPONENT_Y
				: GOWL_INPUT_REMAP_COMPONENT_X, error);
		}
		if (g_ascii_strcasecmp(text, "vertical") != 0
		    && g_ascii_strcasecmp(text, "horizontal") != 0) {
			g_set_error(error, GOWL_INPUT_REMAP_ERROR,
			            GOWL_INPUT_REMAP_ERROR_INVALID,
			            "`scroll: %s' -- vertical or horizontal", text);
			return FALSE;
		}
		return gowl_input_remap_rule_map_scroll(rule, input,
			g_ascii_strcasecmp(text, "horizontal") == 0, error);
	}
	if (yaml_mapping_has_member(m, "stick")) {
		guint side;
		gboolean has_x;
		gboolean has_y;
		gdouble value;

		if (!require_scalar(yaml_mapping_get_member(m, "stick"), "stick",
		                    &text, error)
		    || !parse_side(text, TRUE, &side, error))
			return FALSE;
		has_x = yaml_mapping_has_member(m, "x");
		has_y = yaml_mapping_has_member(m, "y");
		if (GOWL_INPUT_REMAP_INPUT_IS_REL(input) && !has_x && !has_y)
			return gowl_input_remap_rule_map_stick_follow(rule, input, side,
			                                              error);
		if (has_x == has_y) {
			g_set_error(error, GOWL_INPUT_REMAP_ERROR,
			            has_x ? GOWL_INPUT_REMAP_ERROR_NOT_ONE_TO_ONE
			                  : GOWL_INPUT_REMAP_ERROR_INVALID,
			            "the stick target for %s needs exactly one of "
			            "`x:' or `y:' (a diagonal is two keys, which "
			            "combine)", input_name);
			return FALSE;
		}
		if (!parse_unit_number(yaml_mapping_get_member(m, has_x ? "x" : "y"),
		                       has_x ? "x" : "y", -1.0, 1.0, &value, error))
			return FALSE;
		return gowl_input_remap_rule_map_stick(rule, input, side,
			has_x ? GOWL_INPUT_REMAP_COMPONENT_X : GOWL_INPUT_REMAP_COMPONENT_Y,
			value, error);
	}
	if (yaml_mapping_has_member(m, "trigger")) {
		guint side;
		gdouble value;

		if (!require_scalar(yaml_mapping_get_member(m, "trigger"), "trigger",
		                    &text, error)
		    || !parse_side(text, FALSE, &side, error))
			return FALSE;
		value = 1.0;
		if (yaml_mapping_has_member(m, "value")
		    && !parse_unit_number(yaml_mapping_get_member(m, "value"),
		                          "value", 0.0, 1.0, &value, error))
			return FALSE;
		return gowl_input_remap_rule_map_trigger(rule, input, side, value,
		                                         error);
	}
	if (yaml_mapping_has_member(m, "pad-button")) {
		guint32 button;

		if (!require_scalar(yaml_mapping_get_member(m, "pad-button"),
		                    "pad-button", &text, error))
			return FALSE;
		if (!gowl_input_remap_code_from_name(text, &button)
		    || button > KEY_MAX) {
			g_set_error(error, GOWL_INPUT_REMAP_ERROR,
			            GOWL_INPUT_REMAP_ERROR_UNKNOWN_CODE,
			            "`%s' is not a pad button (BTN_SOUTH, pad-a, "
			            "left-bumper, ...)", text);
			return FALSE;
		}
		return gowl_input_remap_rule_map_pad_button(rule, input, button,
		                                            error);
	}
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
	if (yaml_mapping_has_member(m, "macro")) {
		const gchar *margs;
		YamlNode *args_node;

		if (!require_scalar(yaml_mapping_get_member(m, "macro"), "macro",
		                    &text, error))
			return FALSE;
		margs = NULL;
		args_node = yaml_mapping_get_member(m, "args");
		if (args_node != NULL
		    && !require_scalar(args_node, "args", &margs, error))
			return FALSE;
		return gowl_input_remap_rule_map_macro(rule, input, text, margs,
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
			else if (g_ascii_strcasecmp(v, "gamepad") == 0
			         || g_ascii_strcasecmp(v, "controller") == 0)
				rule->match_type = GOWL_INPUT_REMAP_DEVICE_GAMEPAD;
			else if (g_ascii_strcasecmp(v, "joystick") == 0)
				rule->match_type = GOWL_INPUT_REMAP_DEVICE_JOYSTICK;
			else if (g_ascii_strcasecmp(v, "any") == 0)
				rule->match_type = GOWL_INPUT_REMAP_DEVICE_ANY;
			else {
				g_set_error(error, GOWL_INPUT_REMAP_ERROR,
				            GOWL_INPUT_REMAP_ERROR_INVALID,
				            "`type: %s' -- keyboard, pointer, gamepad, "
				            "joystick or any", v);
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

/* A list of names (a YAML sequence, or one comma-separated scalar),
 * each resolved by @resolve into @out. */
static gboolean
parse_name_list(
	YamlNode     *node,
	const gchar  *what,
	gboolean    (*resolve)(const gchar *name, guint32 *out),
	GArray       *out,
	GError      **error
){
	g_auto(GStrv) parts = NULL;
	guint i;
	guint n;

	if (node != NULL && yaml_node_get_node_type(node) == YAML_NODE_SEQUENCE) {
		YamlSequence *seq;

		seq = yaml_node_get_sequence(node);
		n = seq != NULL ? yaml_sequence_get_length(seq) : 0;
		parts = g_new0(gchar *, n + 1);
		for (i = 0; i < n; i++) {
			const gchar *t = node_scalar(yaml_sequence_get_element(seq, i));

			parts[i] = g_strdup(t != NULL ? t : "");
		}
	} else {
		const gchar *t;

		if (!require_scalar(node, what, &t, error))
			return FALSE;
		parts = g_strsplit(t, ",", -1);
	}
	for (i = 0; parts[i] != NULL; i++) {
		guint32 code;

		g_strstrip(parts[i]);
		if (*parts[i] == '\0')
			continue;
		if (!resolve(parts[i], &code)) {
			g_set_error(error, GOWL_INPUT_REMAP_ERROR,
			            GOWL_INPUT_REMAP_ERROR_UNKNOWN_CODE,
			            "`%s' in `%s' is not one", parts[i], what);
			return FALSE;
		}
		g_array_append_val(out, code);
	}
	return TRUE;
}

/* An axis name (ABS_X, left-stick-x) to its ABS code. */
static gboolean
resolve_axis(
	const gchar *name,
	guint32     *out
){
	guint32 input;

	if (!gowl_input_remap_code_from_name(name, &input)
	    || !GOWL_INPUT_REMAP_INPUT_IS_ABS(input))
		return FALSE;
	*out = GOWL_INPUT_REMAP_ABS_OF(input);
	return TRUE;
}

/* A pad button name (BTN_SOUTH, pad-a) to its code. */
static gboolean
resolve_pad_button(
	const gchar *name,
	guint32     *out
){
	guint32 code;

	if (!gowl_input_remap_code_from_name(name, &code) || code > KEY_MAX)
		return FALSE;
	*out = code;
	return TRUE;
}

/* `virtual-pad: {name, id | vendor/product, buttons, axes}'. */
static gboolean
parse_virtual_pad(
	GowlInputRemapRule  *rule,
	YamlNode            *node,
	GError             **error
){
	YamlMapping *m;
	const gchar *name;
	guint16 vendor;
	guint16 product;
	guint n;
	guint i;

	m = node != NULL ? yaml_node_get_mapping(node) : NULL;
	if (m == NULL) {
		g_set_error(error, GOWL_INPUT_REMAP_ERROR,
		            GOWL_INPUT_REMAP_ERROR_INVALID,
		            "`virtual-pad:' is a mapping, {name: NAME}");
		return FALSE;
	}
	n = yaml_mapping_get_size(m);
	for (i = 0; i < n; i++) {
		const gchar *k = yaml_mapping_get_key(m, i);

		if (g_strcmp0(k, "name") != 0 && g_strcmp0(k, "id") != 0
		    && g_strcmp0(k, "vendor") != 0 && g_strcmp0(k, "product") != 0
		    && g_strcmp0(k, "buttons") != 0 && g_strcmp0(k, "axes") != 0) {
			g_set_error(error, GOWL_INPUT_REMAP_ERROR,
			            GOWL_INPUT_REMAP_ERROR_INVALID,
			            "unknown virtual-pad key `%s' (name, id, vendor, "
			            "product, buttons, axes)", k);
			return FALSE;
		}
	}
	if (!require_scalar(yaml_mapping_get_member(m, "name"), "virtual-pad name",
	                    &name, error))
		return FALSE;
	vendor = 0;
	product = 0;
	if (yaml_mapping_has_member(m, "id")) {
		const gchar *v;
		guint a;
		guint b;
		gchar tail;

		if (!require_scalar(yaml_mapping_get_member(m, "id"), "id", &v, error))
			return FALSE;
		if (sscanf(v, "%4x:%4x%c", &a, &b, &tail) != 2) {
			g_set_error(error, GOWL_INPUT_REMAP_ERROR,
			            GOWL_INPUT_REMAP_ERROR_INVALID,
			            "virtual-pad `id: %s' is not VENDOR:PRODUCT in hex", v);
			return FALSE;
		}
		vendor = (guint16)a;
		product = (guint16)b;
	}
	for (i = 0; i < 2; i++) {
		const gchar *key = i == 0 ? "vendor" : "product";
		const gchar *v;
		guint16 num;

		if (!yaml_mapping_has_member(m, key))
			continue;
		if (!require_scalar(yaml_mapping_get_member(m, key), key, &v, error))
			return FALSE;
		if (!parse_hex_id(v, &num)) {
			g_set_error(error, GOWL_INPUT_REMAP_ERROR,
			            GOWL_INPUT_REMAP_ERROR_INVALID,
			            "virtual-pad `%s: %s' is not a 16-bit hex id", key, v);
			return FALSE;
		}
		if (i == 0)
			vendor = num;
		else
			product = num;
	}
	if (!gowl_input_remap_rule_set_virtual_pad(rule, name, vendor, product,
	                                           error))
		return FALSE;
	if (yaml_mapping_has_member(m, "buttons")) {
		g_autoptr(GArray) codes = g_array_new(FALSE, FALSE, sizeof(guint32));

		if (!parse_name_list(yaml_mapping_get_member(m, "buttons"),
		                     "buttons", resolve_pad_button, codes, error)
		    || !gowl_input_remap_rule_set_virtual_pad_buttons(rule,
		           (const guint32 *)(gpointer)codes->data, codes->len, error))
			return FALSE;
	}
	if (yaml_mapping_has_member(m, "axes")) {
		g_autoptr(GArray) codes = g_array_new(FALSE, FALSE, sizeof(guint32));

		if (!parse_name_list(yaml_mapping_get_member(m, "axes"),
		                     "axes", resolve_axis, codes, error)
		    || !gowl_input_remap_rule_set_virtual_pad_axes(rule,
		           (const guint32 *)(gpointer)codes->data, codes->len, error))
			return FALSE;
	}
	return TRUE;
}

/* `analog: {deadzone, press, release, invert, centred, pointer-speed,
 * pointer-curve, pointer-accel, scroll-rate, mouse-gain,
 * mouse-idle-ms, slant, gate}'; validated as a whole by set_analog. */
static gboolean
parse_analog(
	GowlInputRemapRule  *rule,
	YamlNode            *node,
	GError             **error
){
	static const struct {
		const gchar *key;
		gsize        offset;
		gdouble      lo;
		gdouble      hi;
	} nums[] = {
		{ "deadzone",      G_STRUCT_OFFSET(GowlInputRemapAnalog, deadzone),      -1.0, 1.0 },
		{ "press",         G_STRUCT_OFFSET(GowlInputRemapAnalog, press),          0.0, 1.0 },
		{ "release",       G_STRUCT_OFFSET(GowlInputRemapAnalog, release),        0.0, 1.0 },
		{ "pointer-speed", G_STRUCT_OFFSET(GowlInputRemapAnalog, pointer_speed),  0.0, 1e9 },
		{ "pointer-curve", G_STRUCT_OFFSET(GowlInputRemapAnalog, pointer_curve),  0.0, 1e9 },
		{ "pointer-accel", G_STRUCT_OFFSET(GowlInputRemapAnalog, pointer_accel),  0.0, 1e9 },
		{ "scroll-rate",   G_STRUCT_OFFSET(GowlInputRemapAnalog, scroll_rate),    0.0, 1e9 },
		{ "mouse-gain",    G_STRUCT_OFFSET(GowlInputRemapAnalog, mouse_gain),     0.0, 1e9 },
		{ "slant",         G_STRUCT_OFFSET(GowlInputRemapAnalog, slant),         -1e3, 1e3 },
		{ "gate",          G_STRUCT_OFFSET(GowlInputRemapAnalog, gate),           0.0, 1e3 },
	};
	GowlInputRemapAnalog a;
	YamlMapping *m;
	guint n;
	guint i;
	guint j;

	m = node != NULL ? yaml_node_get_mapping(node) : NULL;
	if (m == NULL) {
		g_set_error(error, GOWL_INPUT_REMAP_ERROR,
		            GOWL_INPUT_REMAP_ERROR_INVALID,
		            "`analog:' is a mapping of settings");
		return FALSE;
	}
	gowl_input_remap_rule_get_analog(rule, &a);
	n = yaml_mapping_get_size(m);
	for (i = 0; i < n; i++) {
		const gchar *k = yaml_mapping_get_key(m, i);
		YamlNode *v = yaml_mapping_get_value(m, i);
		gboolean known;

		known = FALSE;
		for (j = 0; j < G_N_ELEMENTS(nums); j++) {
			if (g_strcmp0(k, nums[j].key) != 0)
				continue;
			if (!parse_unit_number(v, k, nums[j].lo, nums[j].hi,
			                       (gdouble *)(gpointer)((guint8 *)&a
			                                             + nums[j].offset),
			                       error))
				return FALSE;
			known = TRUE;
		}
		if (known)
			continue;
		if (g_strcmp0(k, "mouse-idle-ms") == 0) {
			gdouble ms;

			if (!parse_unit_number(v, k, 0.0, 1e6, &ms, error))
				return FALSE;
			a.mouse_idle_ms = (guint)ms;
		} else if (g_strcmp0(k, "invert") == 0
		           || g_strcmp0(k, "centred") == 0
		           || g_strcmp0(k, "centered") == 0) {
			g_autoptr(GArray) axes = g_array_new(FALSE, FALSE, sizeof(guint32));
			guint64 mask;

			if (!parse_name_list(v, k, resolve_axis, axes, error))
				return FALSE;
			mask = 0;
			for (j = 0; j < axes->len; j++)
				mask |= (guint64)1 << g_array_index(axes, guint32, j);
			if (g_strcmp0(k, "invert") == 0)
				a.invert = mask;
			else
				a.centred = mask;
		} else {
			g_set_error(error, GOWL_INPUT_REMAP_ERROR,
			            GOWL_INPUT_REMAP_ERROR_INVALID,
			            "unknown analog key `%s' (deadzone, press, release, "
			            "invert, centred, pointer-speed, pointer-curve, "
			            "pointer-accel, scroll-rate, mouse-gain, "
			            "mouse-idle-ms, slant, gate)", k);
			return FALSE;
		}
	}
	return gowl_input_remap_rule_set_analog(rule, &a, error);
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
		    && g_strcmp0(k, "log") != 0 && g_strcmp0(k, "grab") != 0
		    && g_strcmp0(k, "analog") != 0
		    && g_strcmp0(k, "virtual-pad") != 0) {
			g_set_error(error, GOWL_INPUT_REMAP_ERROR,
			            GOWL_INPUT_REMAP_ERROR_INVALID,
			            "unknown rule key `%s' (name, match, map, "
			            "unmatched, log, grab, analog, virtual-pad)", k);
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

	if (yaml_mapping_has_member(m, "grab")) {
		if (!require_scalar(yaml_mapping_get_member(m, "grab"), "grab", &v,
		                    error))
			return NULL;
		rule->grab = g_ascii_strcasecmp(v, "true") == 0
		             || g_ascii_strcasecmp(v, "yes") == 0
		             || g_strcmp0(v, "1") == 0;
	}
	/* The pad before the map: stick targets are checked against it */
	if (yaml_mapping_has_member(m, "virtual-pad")
	    && !parse_virtual_pad(rule, yaml_mapping_get_member(m, "virtual-pad"),
	                          error))
		return NULL;
	if (yaml_mapping_has_member(m, "analog")
	    && !parse_analog(rule, yaml_mapping_get_member(m, "analog"), error))
		return NULL;

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
