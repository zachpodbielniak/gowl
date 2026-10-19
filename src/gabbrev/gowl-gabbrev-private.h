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
 * gowl-gabbrev-private.h - The expander's side of the gabbrev API.
 * Not installed: the module (and the tests) make requests and read the
 * registries; everybody else only answers and registers.
 */

#ifndef GOWL_GABBREV_PRIVATE_H
#define GOWL_GABBREV_PRIVATE_H

#include "gabbrev/gowl-gabbrev.h"

G_BEGIN_DECLS

/**
 * GowlGabbrevRequestDone:
 * @request: the request that was just answered
 * @user_data: the data given to gowl_gabbrev_request_new()
 *
 * Called once, on whichever thread answered.  The expander's version
 * only queues the request for its own event loop.
 */
typedef void (*GowlGabbrevRequestDone) (GowlGabbrevRequest *request,
                                        gpointer            user_data);

/* What a request describes; every string is copied. */
typedef struct {
	const gchar *var_name;
	const gchar *var_type;
	GHashTable  *params;      /* gchar* -> gchar*, referenced */
	const gchar *params_json;
	GHashTable  *vars;        /* gchar* -> gchar*, referenced */
	const gchar *trigger;
	const gchar *app_id;
	const gchar *title;
} GowlGabbrevRequestInfo;

GowlGabbrevRequest *gowl_gabbrev_request_new          (const GowlGabbrevRequestInfo *info,
                                                       GowlGabbrevRequestDone        done,
                                                       gpointer                      user_data,
                                                       GDestroyNotify                destroy);
const gchar        *gowl_gabbrev_request_get_value    (GowlGabbrevRequest *request);
const gchar        *gowl_gabbrev_request_get_error    (GowlGabbrevRequest *request);
void                gowl_gabbrev_request_cancel       (GowlGabbrevRequest *request);

/* A registered type or function, held for one call: the registry may
   replace it meanwhile, and the destroy notify waits for the release. */
typedef struct _GowlGabbrevHandle GowlGabbrevHandle;

GowlGabbrevHandle  *gowl_gabbrev_lookup_var_type      (const gchar        *type);
GowlGabbrevHandle  *gowl_gabbrev_lookup_func          (const gchar        *name);
void                gowl_gabbrev_handle_call_var      (GowlGabbrevHandle  *handle,
                                                       GowlGabbrevRequest *request);
gchar              *gowl_gabbrev_handle_call_func     (GowlGabbrevHandle  *handle,
                                                       GowlGabbrevRequest *request);
void                gowl_gabbrev_handle_release       (GowlGabbrevHandle  *handle);

/* The process-wide sources, and a counter that moves whenever the
   registries or the sources change, so the module knows to reload. */
typedef struct {
	gboolean  is_file;
	gchar    *text;            /* a path, or YAML */
} GowlGabbrevSource;

GPtrArray          *gowl_gabbrev_dup_sources          (void);
void                gowl_gabbrev_source_free          (GowlGabbrevSource  *source);
guint               gowl_gabbrev_get_generation       (void);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(GowlGabbrevHandle, gowl_gabbrev_handle_release)

G_END_DECLS

#endif /* GOWL_GABBREV_PRIVATE_H */
