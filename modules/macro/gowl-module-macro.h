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
 * gowl-module-macro.h - The macro module's internal pieces.
 */

#ifndef GOWL_MODULE_MACRO_H
#define GOWL_MODULE_MACRO_H

#include <gio/gio.h>
#include <wayland-server-core.h>

G_BEGIN_DECLS

/* The optional org.gowl.Macro1 service (gowl-macro-dbus.c) */
typedef struct _GowlMacroDbus GowlMacroDbus;

/* Answers one method call on the compositor thread; returns the reply
   line (`OK ...' / `ERROR ...'). */
typedef gchar *(*GowlMacroDbusHandler) (gpointer     module,
                                        const gchar *method,
                                        const gchar *name,
                                        GStrv        args);

GowlMacroDbus *gowl_macro_dbus_start        (struct wl_event_loop *loop,
                                             GBusType              bus_type,
                                             GowlMacroDbusHandler  handler,
                                             gpointer              module);
void           gowl_macro_dbus_stop         (GowlMacroDbus        *self);
void           gowl_macro_dbus_emit         (GowlMacroDbus        *self,
                                             const gchar          *signal,
                                             const gchar          *name,
                                             const gchar          *detail);
gboolean       gowl_macro_dbus_is_owned     (GowlMacroDbus        *self);

G_END_DECLS

#endif /* GOWL_MODULE_MACRO_H */
