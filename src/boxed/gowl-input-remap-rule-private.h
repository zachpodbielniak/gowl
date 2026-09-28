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
 * gowl-input-remap-rule-private.h - Not installed.
 *
 * The YAML-node entry point the config loader uses (the public header
 * cannot name yaml-glib types: gowl-config.h does not pull yaml-glib in
 * either), and the callback generation counter the config compiler
 * reads to know whether a compiled C config must stay loaded.
 */

#ifndef GOWL_INPUT_REMAP_RULE_PRIVATE_H
#define GOWL_INPUT_REMAP_RULE_PRIVATE_H

#include "boxed/gowl-input-remap-rule.h"
#include "yaml-glib.h"

G_BEGIN_DECLS

GowlInputRemapRule *gowl_input_remap_rule_new_from_node   (YamlNode  *node,
                                                           GError   **error);
guint               gowl_input_remap_callback_generation  (void);

G_END_DECLS

#endif /* GOWL_INPUT_REMAP_RULE_PRIVATE_H */
