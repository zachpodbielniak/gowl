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
 * @type: keyboard or pointer
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
