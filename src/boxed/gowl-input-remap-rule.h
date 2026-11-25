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
 * gowl-input-remap-rule.h - Per-device input remap rules.
 *
 * A rule says which physical devices it applies to (by libinput name,
 * USB vendor:product, sysname and device kind) and what each of that
 * device's inputs turns into.  The data model is the enforcement of the
 * one-to-one guarantee: an input maps to exactly ONE target, a mapping
 * replaces the previous one, and there is no target kind that holds a
 * list, a delay or a repeat.  See docs/input-remap.org.
 *
 * Rules live in libgowl, not in the input-remap module, because the C
 * config runs before any module is loaded and the YAML `input-remap:'
 * section is parsed by the core config loader.  The module is the
 * engine that applies them.
 */

#ifndef GOWL_INPUT_REMAP_RULE_H
#define GOWL_INPUT_REMAP_RULE_H

#include <glib-object.h>

#include "gowl-types.h"
#include "gowl-enums.h"

G_BEGIN_DECLS

/**
 * GOWL_INPUT_REMAP_ERROR:
 *
 * Error domain for input-remap rule construction and parsing.
 */
#define GOWL_INPUT_REMAP_ERROR (gowl_input_remap_error_quark())

/**
 * GowlInputRemapError:
 * @GOWL_INPUT_REMAP_ERROR_INVALID: the rule is malformed (missing
 *   name, unknown key, wrong value type).
 * @GOWL_INPUT_REMAP_ERROR_NOT_ONE_TO_ONE: the rule asks for more than
 *   one output per input -- a sequence, a macro, a delay, a repeat, or
 *   more than one target kind for the same input.  Refused outright.
 * @GOWL_INPUT_REMAP_ERROR_UNKNOWN_CODE: an input or target name does
 *   not resolve to a key, button, keysym or action.
 *
 * Why a rule was refused.
 */
typedef enum {
	GOWL_INPUT_REMAP_ERROR_INVALID,
	GOWL_INPUT_REMAP_ERROR_NOT_ONE_TO_ONE,
	GOWL_INPUT_REMAP_ERROR_UNKNOWN_CODE
} GowlInputRemapError;

GQuark gowl_input_remap_error_quark (void);

/*
 * Wheel notches, as input codes.  The same values `mousebinds:' uses
 * (gowl-keybind.h), outside the evdev key/button range so one guint32
 * names any input a rule can match.
 */
#ifndef GOWL_BUTTON_WHEEL_UP
#define GOWL_BUTTON_WHEEL_UP    (0x10000)
#define GOWL_BUTTON_WHEEL_DOWN  (0x10001)
#define GOWL_BUTTON_WHEEL_LEFT  (0x10002)
#define GOWL_BUTTON_WHEEL_RIGHT (0x10003)
#endif

/*
 * Game-controller inputs, as input codes, above the wheel notches.
 *
 * An absolute axis is three inputs: the WHOLE axis (continuous, for the
 * `pointer:' and `scroll:' targets), and its positive and negative
 * DIRECTIONS (digital: pressed while the stick is pushed past the
 * rule's press threshold, released below its release threshold).
 * `ABS_X' names the whole axis, `ABS_X+' and `ABS_X-' its directions;
 * a bare axis name given a digital target means its positive direction
 * (a trigger pulled).  Pad buttons need no new codes: BTN_SOUTH and
 * friends are evdev key codes already.
 *
 * Pointer motion on a claimed pointer is two inputs, REL_X and REL_Y,
 * which only a stick-follow target (mouse-look onto a virtual stick)
 * can take.
 */
#define GOWL_INPUT_REMAP_ABS_COUNT   (64)
#define GOWL_INPUT_REMAP_ABS_BASE    (0x20000)
#define GOWL_INPUT_REMAP_ABS_WHOLE   (0)
#define GOWL_INPUT_REMAP_ABS_POS     (1)
#define GOWL_INPUT_REMAP_ABS_NEG     (2)
#define GOWL_INPUT_REMAP_ABS_INPUT(abs, dir) \
	((guint32)(GOWL_INPUT_REMAP_ABS_BASE + ((guint32)(abs) << 2) + (guint32)(dir)))
#define GOWL_INPUT_REMAP_INPUT_IS_ABS(input) \
	((input) >= GOWL_INPUT_REMAP_ABS_BASE \
	 && (input) < GOWL_INPUT_REMAP_ABS_BASE + (GOWL_INPUT_REMAP_ABS_COUNT << 2) \
	 && (((input) - GOWL_INPUT_REMAP_ABS_BASE) & 3) != 3)
#define GOWL_INPUT_REMAP_ABS_OF(input)  (((input) - GOWL_INPUT_REMAP_ABS_BASE) >> 2)
#define GOWL_INPUT_REMAP_ABS_DIR(input) (((input) - GOWL_INPUT_REMAP_ABS_BASE) & 3)
#define GOWL_INPUT_REMAP_REL_X       (0x30000)
#define GOWL_INPUT_REMAP_REL_Y       (0x30001)
#define GOWL_INPUT_REMAP_INPUT_IS_REL(input) \
	((input) == GOWL_INPUT_REMAP_REL_X || (input) == GOWL_INPUT_REMAP_REL_Y)

/* Virtual-pad sides and components, for stick and trigger targets. */
#define GOWL_INPUT_REMAP_SIDE_LEFT   (0)
#define GOWL_INPUT_REMAP_SIDE_RIGHT  (1)
#define GOWL_INPUT_REMAP_SIDE_DPAD   (2)
#define GOWL_INPUT_REMAP_COMPONENT_X (0)
#define GOWL_INPUT_REMAP_COMPONENT_Y (1)

/* The id a virtual pad gets when its declaration names none: pid.codes'
 * test range, which no shipping product uses. */
#define GOWL_INPUT_REMAP_PAD_VENDOR  (0x1209)
#define GOWL_INPUT_REMAP_PAD_PRODUCT (0x0007)
/* Every virtual pad's device name starts with this, which is also how
 * gowl recognises its own pads and never reads one back as an input. */
#define GOWL_INPUT_REMAP_PAD_NAME_PREFIX "gowl virtual pad: "

/**
 * GowlInputRemapAnalog:
 * @deadzone: the deadzone, [0, 0.95]; negative means "from the
 *   device's own flat" (the default)
 * @press: the deflection that presses a stick direction, (0, 1]
 *   (default 0.5)
 * @release: the deflection below which it releases, [0, @press)
 *   (default 0.35)
 * @invert: one bit per ABS code: axes read reversed
 * @centred: one bit per ABS code: axes read as centred even though
 *   their range starts at 0 (a cheap pad's right stick on ABS_Z)
 * @pointer_speed: pointer speed at full deflection, pixels per second
 *   (default 1200)
 * @pointer_curve: the speed curve exponent (default 2.0)
 * @pointer_accel: extra speed per second held, as a fraction of
 *   @pointer_speed (default 0: none)
 * @scroll_rate: scroll notches per second at full deflection (default 8)
 * @mouse_gain: stick deflection per pixel of mouse motion, for a
 *   stick-follow target (default 0.02)
 * @mouse_idle_ms: a mouse-driven stick returns to centre this long after
 *   the last motion (default 100)
 * @slant: degrees toward +X that a straight vertical push reads as.
 *   Taken off X in proportion to |Y| (see gowl_remap_analog_slant()).
 *   0, the default, leaves the stick alone.  Not a rotation
 * @gate: lesser/greater ratio below which a stick direction is not
 *   pressed (see gowl_remap_analog_gate()).  0, the default, keeps
 *   every axis that crosses @press, so a diagonal is two keys
 *
 * The analog settings of one rule: how it reads a controller's axes
 * and how fast its continuous targets run.
 */
typedef struct {
	gdouble deadzone;
	gdouble press;
	gdouble release;
	guint64 invert;
	guint64 centred;
	gdouble pointer_speed;
	gdouble pointer_curve;
	gdouble pointer_accel;
	gdouble scroll_rate;
	gdouble mouse_gain;
	guint   mouse_idle_ms;
	gdouble slant;
	gdouble gate;
} GowlInputRemapAnalog;

/* --- GowlInputRemapEvent --- */

#define GOWL_TYPE_INPUT_REMAP_EVENT (gowl_input_remap_event_get_type())

/**
 * GowlInputRemapEvent:
 * @kind: key, button or wheel notch
 * @code: the evdev code (KEY_* / BTN_*) or a %GOWL_BUTTON_WHEEL_* value
 * @pressed: %TRUE for a press, %FALSE for its release
 * @time_msec: the event timestamp in milliseconds
 * @device_id: the compositor-assigned id of the device it came from
 *   (see #GowlInputDeviceInfo)
 * @value: for %GOWL_INPUT_REMAP_EVENT_MOTION the motion in pixels; for
 *   a %GOWL_INPUT_REMAP_EVENT_PAD axis direction the normalised
 *   deflection that pressed or released it; otherwise 0
 *
 * One physical input on a claimed device, as handed to a
 * #GowlInputRemapper and to a %GOWL_INPUT_REMAP_TARGET_CALLBACK.
 */
typedef struct _GowlInputRemapEvent GowlInputRemapEvent;

struct _GowlInputRemapEvent {
	GowlInputRemapEventKind kind;
	guint32                 code;
	gboolean                pressed;
	guint32                 time_msec;
	guint                   device_id;
	gdouble                 value;
};

GType                gowl_input_remap_event_get_type (void) G_GNUC_CONST;
GowlInputRemapEvent *gowl_input_remap_event_copy     (const GowlInputRemapEvent *self);
void                 gowl_input_remap_event_free     (GowlInputRemapEvent       *self);

/* --- GowlInputDeviceInfo --- */

#define GOWL_TYPE_INPUT_DEVICE_INFO (gowl_input_device_info_get_type())

/**
 * GowlInputDeviceInfo:
 * @id: compositor-assigned id, stable for as long as the device stays
 *   plugged in
 * @type: keyboard, pointer, gamepad or joystick
 * @name: (nullable): the device name libinput reports
 * @sysname: (nullable): the kernel sysname ("event5"), or %NULL for a
 *   device that is not a libinput device (headless, virtual)
 * @vendor: USB/HID vendor id, 0 when unknown
 * @product: USB/HID product id, 0 when unknown
 * @claimed: whether a remapper has claimed the device
 *
 * The identity of one connected input device: everything a remap rule
 * can match on.  A plain value, carrying no wlroots state, so rules can
 * be matched and tested without a running compositor.
 */
typedef struct _GowlInputDeviceInfo GowlInputDeviceInfo;

struct _GowlInputDeviceInfo {
	guint                    id;
	GowlInputRemapDeviceType type;
	gchar                   *name;
	gchar                   *sysname;
	guint16                  vendor;
	guint16                  product;
	gboolean                 claimed;
};

GType                gowl_input_device_info_get_type (void) G_GNUC_CONST;
GowlInputDeviceInfo *gowl_input_device_info_new      (guint                     id,
                                                      GowlInputRemapDeviceType  type,
                                                      const gchar              *name,
                                                      const gchar              *sysname,
                                                      guint16                   vendor,
                                                      guint16                   product);
GowlInputDeviceInfo *gowl_input_device_info_copy     (const GowlInputDeviceInfo *self);
void                 gowl_input_device_info_free     (GowlInputDeviceInfo       *self);
gchar               *gowl_input_device_info_to_string(const GowlInputDeviceInfo *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(GowlInputDeviceInfo, gowl_input_device_info_free)

/* --- GowlInputRemapRule / GowlInputRemapTarget --- */

#define GOWL_TYPE_INPUT_REMAP_RULE (gowl_input_remap_rule_get_type())

typedef struct _GowlInputRemapRule   GowlInputRemapRule;
typedef struct _GowlInputRemapTarget GowlInputRemapTarget;

/**
 * GowlInputRemapCallback:
 * @rule: the rule the input matched
 * @event: the physical input
 * @compositor: (nullable): the compositor, for reaching the session
 * @user_data: the data given to gowl_input_remap_rule_map_callback()
 *
 * Arbitrary code run for one input, on press and again on release,
 * on the compositor thread.  Nothing constrains what it does: the
 * one-to-one guarantee covers the declarative targets only.  Keep it
 * short -- the whole compositor waits for it.
 */
typedef void (*GowlInputRemapCallback) (GowlInputRemapRule        *rule,
                                        const GowlInputRemapEvent *event,
                                        GowlCompositor            *compositor,
                                        gpointer                   user_data);

GType               gowl_input_remap_rule_get_type    (void) G_GNUC_CONST;
GowlInputRemapRule *gowl_input_remap_rule_new         (const gchar        *name);
GowlInputRemapRule *gowl_input_remap_rule_ref         (GowlInputRemapRule *self);
void                gowl_input_remap_rule_unref       (GowlInputRemapRule *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(GowlInputRemapRule, gowl_input_remap_rule_unref)

const gchar        *gowl_input_remap_rule_get_name    (GowlInputRemapRule *self);

/* Matching */
void                gowl_input_remap_rule_set_match_name     (GowlInputRemapRule       *self,
                                                              const gchar              *glob);
const gchar        *gowl_input_remap_rule_get_match_name     (GowlInputRemapRule       *self);
void                gowl_input_remap_rule_set_match_sysname  (GowlInputRemapRule       *self,
                                                              const gchar              *glob);
const gchar        *gowl_input_remap_rule_get_match_sysname  (GowlInputRemapRule       *self);
void                gowl_input_remap_rule_set_match_ids      (GowlInputRemapRule       *self,
                                                              guint16                   vendor,
                                                              guint16                   product);
guint16             gowl_input_remap_rule_get_match_vendor   (GowlInputRemapRule       *self);
guint16             gowl_input_remap_rule_get_match_product  (GowlInputRemapRule       *self);
void                gowl_input_remap_rule_set_match_type     (GowlInputRemapRule       *self,
                                                              GowlInputRemapDeviceType  type);
GowlInputRemapDeviceType
                    gowl_input_remap_rule_get_match_type     (GowlInputRemapRule       *self);
gboolean            gowl_input_remap_rule_has_match          (GowlInputRemapRule       *self);
gboolean            gowl_input_remap_rule_matches            (GowlInputRemapRule        *self,
                                                              const GowlInputDeviceInfo *info);

/* Behaviour */
void                gowl_input_remap_rule_set_drop_unmatched (GowlInputRemapRule *self,
                                                              gboolean            drop);
gboolean            gowl_input_remap_rule_get_drop_unmatched (GowlInputRemapRule *self);
void                gowl_input_remap_rule_set_log            (GowlInputRemapRule *self,
                                                              gboolean            log);
gboolean            gowl_input_remap_rule_get_log            (GowlInputRemapRule *self);

/* Game controllers and virtual pads */
void                gowl_input_remap_rule_set_grab           (GowlInputRemapRule         *self,
                                                              gboolean                    grab);
gboolean            gowl_input_remap_rule_get_grab           (GowlInputRemapRule         *self);
void                gowl_input_remap_rule_get_analog         (GowlInputRemapRule         *self,
                                                              GowlInputRemapAnalog       *out);
gboolean            gowl_input_remap_rule_set_analog         (GowlInputRemapRule         *self,
                                                              const GowlInputRemapAnalog *analog,
                                                              GError                    **error);
void                gowl_input_remap_analog_init             (GowlInputRemapAnalog       *analog);
gboolean            gowl_input_remap_rule_set_virtual_pad    (GowlInputRemapRule         *self,
                                                              const gchar                *name,
                                                              guint16                     vendor,
                                                              guint16                     product,
                                                              GError                    **error);
const gchar        *gowl_input_remap_rule_get_virtual_pad    (GowlInputRemapRule         *self);
guint16             gowl_input_remap_rule_get_virtual_pad_vendor  (GowlInputRemapRule    *self);
guint16             gowl_input_remap_rule_get_virtual_pad_product (GowlInputRemapRule    *self);
gboolean            gowl_input_remap_rule_set_virtual_pad_buttons (GowlInputRemapRule    *self,
                                                                   const guint32         *buttons,
                                                                   guint                  n_buttons,
                                                                   GError               **error);
GArray             *gowl_input_remap_rule_get_virtual_pad_buttons (GowlInputRemapRule    *self);
gboolean            gowl_input_remap_rule_set_virtual_pad_axes    (GowlInputRemapRule    *self,
                                                                   const guint32         *axes,
                                                                   guint                  n_axes,
                                                                   GError               **error);
GArray             *gowl_input_remap_rule_get_virtual_pad_axes    (GowlInputRemapRule    *self);
gboolean            gowl_input_remap_rule_has_continuous     (GowlInputRemapRule         *self);

/* Mappings: each replaces whatever @input mapped to before */
gboolean            gowl_input_remap_rule_map_key      (GowlInputRemapRule     *self,
                                                        guint32                 input,
                                                        guint32                 keycode,
                                                        guint32                 keysym,
                                                        guint32                 modifiers,
                                                        GError                **error);
gboolean            gowl_input_remap_rule_map_button   (GowlInputRemapRule     *self,
                                                        guint32                 input,
                                                        guint32                 button,
                                                        GError                **error);
gboolean            gowl_input_remap_rule_map_action   (GowlInputRemapRule     *self,
                                                        guint32                 input,
                                                        GowlAction              action,
                                                        const gchar            *arg,
                                                        GError                **error);
gboolean            gowl_input_remap_rule_map_command  (GowlInputRemapRule     *self,
                                                        guint32                 input,
                                                        const gchar            *command,
                                                        GError                **error);
void                gowl_input_remap_rule_map_callback (GowlInputRemapRule     *self,
                                                        guint32                 input,
                                                        GowlInputRemapCallback  callback,
                                                        gpointer                user_data,
                                                        GDestroyNotify          destroy);
gboolean            gowl_input_remap_rule_map_macro    (GowlInputRemapRule     *self,
                                                        guint32                 input,
                                                        const gchar            *name,
                                                        const gchar            *args,
                                                        GError                **error);
gboolean            gowl_input_remap_rule_map_pointer  (GowlInputRemapRule     *self,
                                                        guint32                 input,
                                                        guint                   component,
                                                        GError                **error);
gboolean            gowl_input_remap_rule_map_scroll   (GowlInputRemapRule     *self,
                                                        guint32                 input,
                                                        gboolean                horizontal,
                                                        GError                **error);
gboolean            gowl_input_remap_rule_map_stick    (GowlInputRemapRule     *self,
                                                        guint32                 input,
                                                        guint                   side,
                                                        guint                   component,
                                                        gdouble                 value,
                                                        GError                **error);
gboolean            gowl_input_remap_rule_map_stick_follow (GowlInputRemapRule *self,
                                                        guint32                 input,
                                                        guint                   side,
                                                        GError                **error);
gboolean            gowl_input_remap_rule_map_trigger  (GowlInputRemapRule     *self,
                                                        guint32                 input,
                                                        guint                   side,
                                                        gdouble                 value,
                                                        GError                **error);
gboolean            gowl_input_remap_rule_map_pad_button (GowlInputRemapRule   *self,
                                                        guint32                 input,
                                                        guint32                 button,
                                                        GError                **error);
void                gowl_input_remap_rule_map_drop     (GowlInputRemapRule     *self,
                                                        guint32                 input);
void                gowl_input_remap_rule_map_pass     (GowlInputRemapRule     *self,
                                                        guint32                 input);
gboolean            gowl_input_remap_rule_unmap        (GowlInputRemapRule     *self,
                                                        guint32                 input);
guint               gowl_input_remap_rule_get_n_mappings (GowlInputRemapRule   *self);
GArray             *gowl_input_remap_rule_get_inputs   (GowlInputRemapRule     *self);
const GowlInputRemapTarget *
                    gowl_input_remap_rule_lookup       (GowlInputRemapRule     *self,
                                                        guint32                 input);

/* Targets (owned by their rule; valid while the rule is referenced) */
GowlInputRemapTargetKind gowl_input_remap_target_get_kind      (const GowlInputRemapTarget *self);
guint32                  gowl_input_remap_target_get_code      (const GowlInputRemapTarget *self);
guint32                  gowl_input_remap_target_get_keysym    (const GowlInputRemapTarget *self);
guint32                  gowl_input_remap_target_get_modifiers (const GowlInputRemapTarget *self);
GowlAction               gowl_input_remap_target_get_action    (const GowlInputRemapTarget *self);
const gchar             *gowl_input_remap_target_get_arg       (const GowlInputRemapTarget *self);
const gchar             *gowl_input_remap_target_get_macro_args(const GowlInputRemapTarget *self);
guint                    gowl_input_remap_target_get_side      (const GowlInputRemapTarget *self);
guint                    gowl_input_remap_target_get_component (const GowlInputRemapTarget *self);
gdouble                  gowl_input_remap_target_get_value     (const GowlInputRemapTarget *self);
gboolean                 gowl_input_remap_target_is_follow     (const GowlInputRemapTarget *self);
gboolean                 gowl_input_remap_target_is_continuous (const GowlInputRemapTarget *self);
gboolean                 gowl_input_remap_target_is_pad        (const GowlInputRemapTarget *self);
void                     gowl_input_remap_target_invoke        (const GowlInputRemapTarget *self,
                                                                GowlInputRemapRule         *rule,
                                                                const GowlInputRemapEvent  *event,
                                                                GowlCompositor             *compositor);
gchar                   *gowl_input_remap_target_to_string     (const GowlInputRemapTarget *self);

/* Text forms */
GowlInputRemapRule *gowl_input_remap_rule_new_from_yaml (const gchar        *yaml,
                                                         GError            **error);
GPtrArray          *gowl_input_remap_rules_from_yaml    (const gchar        *yaml,
                                                         GError            **error);
gchar              *gowl_input_remap_rule_to_yaml       (GowlInputRemapRule *self);

/* Input names */
gboolean            gowl_input_remap_code_from_name     (const gchar        *name,
                                                         guint32            *out_code);
gchar              *gowl_input_remap_code_to_name       (guint32             code);
gboolean            gowl_input_remap_button_from_name   (const gchar        *name,
                                                         guint32            *out_button);

G_END_DECLS

#endif /* GOWL_INPUT_REMAP_RULE_H */
