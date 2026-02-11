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

#ifndef GOWL_CLIENT_H
#define GOWL_CLIENT_H

#include <glib-object.h>

G_BEGIN_DECLS

#define GOWL_TYPE_CLIENT (gowl_client_get_type())

G_DECLARE_FINAL_TYPE(GowlClient, gowl_client, GOWL, CLIENT, GObject)

GowlClient    *gowl_client_new               (void);

/* Tag accessors */
guint32        gowl_client_get_tags           (GowlClient  *self);
void           gowl_client_set_tags           (GowlClient  *self,
                                               guint32      tags);

/* State accessors */
gboolean       gowl_client_get_floating       (GowlClient  *self);
void           gowl_client_set_floating       (GowlClient  *self,
                                               gboolean     floating);

gboolean       gowl_client_get_fullscreen     (GowlClient  *self);
void           gowl_client_set_fullscreen     (GowlClient  *self,
                                               gboolean     fullscreen);

gboolean       gowl_client_get_urgent         (GowlClient  *self);
void           gowl_client_set_urgent         (GowlClient  *self,
                                               gboolean     urgent);

/* String accessors */
const gchar   *gowl_client_get_title          (GowlClient  *self);
void           gowl_client_set_title          (GowlClient  *self,
                                               const gchar *title);

const gchar   *gowl_client_get_app_id         (GowlClient  *self);
void           gowl_client_set_app_id         (GowlClient  *self,
                                               const gchar *app_id);

/* Geometry accessors */
void           gowl_client_get_geometry       (GowlClient  *self,
                                               gint        *x,
                                               gint        *y,
                                               gint        *width,
                                               gint        *height);
void           gowl_client_set_geometry       (GowlClient  *self,
                                               gint         x,
                                               gint         y,
                                               gint         width,
                                               gint         height);

/* Monitor accessor */
gpointer       gowl_client_get_monitor        (GowlClient  *self);
void           gowl_client_set_monitor        (GowlClient  *self,
                                               gpointer     monitor);

G_END_DECLS

#endif /* GOWL_CLIENT_H */
