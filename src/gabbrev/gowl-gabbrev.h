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
 * gowl-gabbrev.h - The text expander's public API.
 *
 * gabbrev is gowl's espanso: type `;today' anywhere and it becomes
 * 2026-10-19.  The expander itself is the opt-in `gabbrev' module
 * (docs/gabbrev.org); this header is the part of it that lives in
 * libgowl, so that code which is NOT the module -- a C config, an
 * embedder such as cmacs, a macro -- can extend it whether or not the
 * module is loaded yet:
 *
 *   - VARIABLE TYPES.  A match's `vars:' name a type (`date', `shell',
 *     `clipboard', ...).  gowl_gabbrev_register_var_type() adds one, or
 *     replaces a built-in one: cmacs registers `elisp', and `choice' and
 *     `form' so that pickers are Emacs's minibuffer.  A type is
 *     ASYNCHRONOUS: its function is handed a #GowlGabbrevRequest and
 *     answers it, now or later, from any thread.
 *
 *   - C FUNCTIONS.  gowl_gabbrev_register_func() names a plain C
 *     function a YAML variable can call: `{type: c, params: {func: NAME}}'.
 *
 *   - SOURCES.  gowl_gabbrev_add_file() and gowl_gabbrev_add_yaml() add
 *     match files, espanso config directories, or YAML text.  They are
 *     process-wide and outlive a config reload, which a C config needs:
 *     it runs once, at startup, and a reload builds a fresh #GowlConfig.
 *
 * Everything here is thread-safe.
 */

#ifndef GOWL_GABBREV_H
#define GOWL_GABBREV_H

#include <glib-object.h>

G_BEGIN_DECLS

#define GOWL_TYPE_GABBREV_REQUEST (gowl_gabbrev_request_get_type())

/**
 * GowlGabbrevRequest:
 *
 * One variable waiting for its value.  Opaque and reference counted;
 * the expander holds one reference for as long as it waits, and a
 * provider that answers later takes its own.
 *
 * A request is answered exactly once: the first
 * gowl_gabbrev_request_return() or gowl_gabbrev_request_return_error()
 * wins and later ones are ignored, which is what lets the expander
 * give up on a slow provider (its timeout answers with an error) and
 * the provider still answer late without harm.
 */
typedef struct _GowlGabbrevRequest GowlGabbrevRequest;

/**
 * GowlGabbrevVarFunc:
 * @request: the variable to resolve
 * @user_data: the data given to gowl_gabbrev_register_var_type()
 *
 * Resolves one variable of a registered type.  Called on the
 * compositor thread, which must not block: answer at once with
 * gowl_gabbrev_request_return(), or take a reference with
 * gowl_gabbrev_request_ref() and answer later from any thread.
 */
typedef void   (*GowlGabbrevVarFunc) (GowlGabbrevRequest *request,
                                      gpointer            user_data);

/**
 * GowlGabbrevFunc:
 * @request: the variable to resolve
 * @user_data: the data given to gowl_gabbrev_register_func()
 *
 * A plain C function a `c' variable names with `params: {func: NAME}'.
 * Called on the compositor thread under the fault guard and the
 * variable's time budget, so it must be quick.
 *
 * Returns: (transfer full) (nullable): the value, or %NULL for an error
 */
typedef gchar *(*GowlGabbrevFunc)    (GowlGabbrevRequest *request,
                                      gpointer            user_data);

GType               gowl_gabbrev_request_get_type      (void) G_GNUC_CONST;

GowlGabbrevRequest *gowl_gabbrev_request_ref           (GowlGabbrevRequest *request);
void                gowl_gabbrev_request_unref         (GowlGabbrevRequest *request);

guint64             gowl_gabbrev_request_get_id        (GowlGabbrevRequest *request);
const gchar        *gowl_gabbrev_request_get_var_name  (GowlGabbrevRequest *request);
const gchar        *gowl_gabbrev_request_get_var_type  (GowlGabbrevRequest *request);
const gchar        *gowl_gabbrev_request_get_param     (GowlGabbrevRequest *request,
                                                        const gchar        *key);
GHashTable         *gowl_gabbrev_request_get_params    (GowlGabbrevRequest *request);
const gchar        *gowl_gabbrev_request_get_params_json (GowlGabbrevRequest *request);
const gchar        *gowl_gabbrev_request_get_var       (GowlGabbrevRequest *request,
                                                        const gchar        *name);
GHashTable         *gowl_gabbrev_request_get_vars      (GowlGabbrevRequest *request);
const gchar        *gowl_gabbrev_request_get_trigger   (GowlGabbrevRequest *request);
const gchar        *gowl_gabbrev_request_get_app_id    (GowlGabbrevRequest *request);
const gchar        *gowl_gabbrev_request_get_title     (GowlGabbrevRequest *request);

gboolean            gowl_gabbrev_request_return        (GowlGabbrevRequest *request,
                                                        const gchar        *value);
gboolean            gowl_gabbrev_request_return_error  (GowlGabbrevRequest *request,
                                                        const gchar        *message);
gboolean            gowl_gabbrev_request_is_answered   (GowlGabbrevRequest *request);
gboolean            gowl_gabbrev_request_return_by_id  (guint64             id,
                                                        const gchar        *value,
                                                        const gchar        *error_message);

/* --- extending it --- */
void                gowl_gabbrev_register_var_type     (const gchar        *type,
                                                        GowlGabbrevVarFunc  func,
                                                        gpointer            user_data,
                                                        GDestroyNotify      destroy);
gboolean            gowl_gabbrev_unregister_var_type   (const gchar        *type);
GStrv               gowl_gabbrev_list_var_types        (void);

void                gowl_gabbrev_register_func         (const gchar        *name,
                                                        GowlGabbrevFunc     func,
                                                        gpointer            user_data,
                                                        GDestroyNotify      destroy);
gboolean            gowl_gabbrev_unregister_func       (const gchar        *name);
GStrv               gowl_gabbrev_list_funcs            (void);

/* --- sources that outlive a config reload --- */
void                gowl_gabbrev_add_file              (const gchar        *path);
gboolean            gowl_gabbrev_add_yaml              (const gchar        *yaml,
                                                        GError            **error);
void                gowl_gabbrev_clear_sources         (void);
gboolean            gowl_gabbrev_add_yaml_source       (const gchar        *id,
                                                        const gchar        *yaml,
                                                        GError            **error);
gboolean            gowl_gabbrev_remove_source         (const gchar        *id);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(GowlGabbrevRequest, gowl_gabbrev_request_unref)

G_END_DECLS

#endif /* GOWL_GABBREV_H */
