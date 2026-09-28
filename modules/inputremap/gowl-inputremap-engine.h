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
 * gowl-inputremap-engine.h - The rule set behind the inputremap module.
 *
 * Pure data: no wlroots, no compositor, so the whole decision (which
 * rule claims a device, what an input maps to, which rule wins) can be
 * tested with plain GowlInputDeviceInfo values.
 *
 * Two layers: the CONFIG rules (a snapshot of the config's
 * `input-remap:' list, replaced wholesale on reload) and the RUNTIME
 * rules (added and removed over IPC, MCP or Elisp).  A runtime rule
 * shadows a config rule of the same name.  Among the rules that match a
 * device, the most recently added wins: runtime before config, later
 * before earlier.
 */

#ifndef GOWL_INPUTREMAP_ENGINE_H
#define GOWL_INPUTREMAP_ENGINE_H

#include <glib.h>
#include "boxed/gowl-input-remap-rule.h"

G_BEGIN_DECLS

typedef struct _GowlInputRemapEngine GowlInputRemapEngine;

/**
 * GowlInputRemapRuleSource:
 * @GOWL_INPUT_REMAP_SOURCE_CONFIG: from the config's `input-remap:'
 * @GOWL_INPUT_REMAP_SOURCE_RUNTIME: added at runtime
 *
 * Where a rule in the engine came from.
 */
typedef enum {
	GOWL_INPUT_REMAP_SOURCE_CONFIG,
	GOWL_INPUT_REMAP_SOURCE_RUNTIME
} GowlInputRemapRuleSource;

GowlInputRemapEngine *gowl_input_remap_engine_new              (void);
void                  gowl_input_remap_engine_free             (GowlInputRemapEngine        *self);

void                  gowl_input_remap_engine_set_config_rules (GowlInputRemapEngine        *self,
                                                                GPtrArray                   *rules);
gboolean              gowl_input_remap_engine_add              (GowlInputRemapEngine        *self,
                                                                GowlInputRemapRule          *rule);
gboolean              gowl_input_remap_engine_remove           (GowlInputRemapEngine        *self,
                                                                const gchar                 *name,
                                                                GowlInputRemapRuleSource    *out_source);
guint                 gowl_input_remap_engine_clear_runtime    (GowlInputRemapEngine        *self);

guint                 gowl_input_remap_engine_get_n_rules      (GowlInputRemapEngine        *self);
GPtrArray            *gowl_input_remap_engine_get_rules        (GowlInputRemapEngine        *self);
GowlInputRemapRuleSource
                      gowl_input_remap_engine_get_source       (GowlInputRemapEngine        *self,
                                                                GowlInputRemapRule          *rule);

gboolean              gowl_input_remap_engine_claims           (GowlInputRemapEngine        *self,
                                                                const GowlInputDeviceInfo   *info);
GowlInputRemapRule   *gowl_input_remap_engine_map              (GowlInputRemapEngine        *self,
                                                                const GowlInputDeviceInfo   *info,
                                                                guint32                      input,
                                                                const GowlInputRemapTarget **out_target);

G_END_DECLS

#endif /* GOWL_INPUTREMAP_ENGINE_H */
