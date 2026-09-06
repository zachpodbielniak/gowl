/* Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later */

#ifndef GOWL_EFFECTS_H
#define GOWL_EFFECTS_H
#include "interfaces/gowl-scene-effect.h"

gboolean gowl_effects_client_event (GowlCompositor *, GowlClient *,
                                    GowlSceneEffectEvent, const struct wlr_box *,
                                    gboolean);
struct wlr_box gowl_effects_geometry (GowlClient *);
gboolean gowl_effects_has_geometry (GowlClient *);
void gowl_effects_alpha_changed (GowlClient *, gfloat);
struct wlr_surface *gowl_effects_surface_at (GowlClient *, gdouble, gdouble, gdouble *,
                                             gdouble *);
gboolean gowl_effects_frame (GowlCompositor *, GowlMonitor *, gint64);
void gowl_effects_frame_done (GowlCompositor *, GowlMonitor *, const struct timespec *);
void gowl_effects_monitor_removed (GowlCompositor *, GowlMonitor *);
void gowl_effects_finish (GowlCompositor *);
#endif
