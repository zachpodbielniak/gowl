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
 * gowl-broadcast-private.h - Not installed.
 *
 * The YAML-node entry points the config loader uses: the public header
 * cannot name yaml-glib types, for the same reason the input-remap rule
 * keeps its node constructor private.
 */

#ifndef GOWL_BROADCAST_PRIVATE_H
#define GOWL_BROADCAST_PRIVATE_H

#include "boxed/gowl-broadcast.h"
#include "yaml-glib.h"

G_BEGIN_DECLS

GowlBroadcastTeam *gowl_broadcast_team_new_from_node (YamlNode  *node,
                                                      GError   **error);
GowlBroadcastRule *gowl_broadcast_rule_new_from_node (YamlNode  *node,
                                                      GError   **error);

G_END_DECLS

#endif /* GOWL_BROADCAST_PRIVATE_H */
