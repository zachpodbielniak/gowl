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

#ifndef GOWL_CAPTURE_SOURCE_H
#define GOWL_CAPTURE_SOURCE_H

#include <glib-object.h>

G_BEGIN_DECLS

#define GOWL_TYPE_CAPTURE_SOURCE (gowl_capture_source_get_type())

/**
 * GowlCaptureSourceKind:
 * @GOWL_CAPTURE_SOURCE_MONITOR: a whole output (monitor) -- always
 *   available (wlr-screencopy, wlroots 0.19+).
 * @GOWL_CAPTURE_SOURCE_WINDOW: a single toplevel window -- only when the
 *   capture provider advertises window capture (wlroots 0.20+, the
 *   ext-image-copy-capture + foreign-toplevel stack).
 *
 * What a #GowlCaptureSource refers to.  Kept deliberately small and
 * version-independent so the boxed type carries no wlroots types and can
 * be used from tests, GI, and either capture backend.
 */
typedef enum {
	GOWL_CAPTURE_SOURCE_MONITOR,
	GOWL_CAPTURE_SOURCE_WINDOW
} GowlCaptureSourceKind;

/**
 * GowlCaptureSource:
 * @kind: monitor or window (see #GowlCaptureSourceKind).
 * @id: stable identifier -- the output name for a monitor, or the
 *   foreign-toplevel identifier string for a window.
 * @title: human-readable title (window title, or monitor name/desc);
 *   may be %NULL.
 * @app_id: application id for a window source; %NULL for monitors.
 *
 * A screencast-capturable entity surfaced by a #GowlCaptureProvider.
 * This is a plain value type (boxed) deliberately free of any wlroots
 * struct, so the same description crosses the provider interface, the
 * IPC/event surface, and the test suite regardless of which wlroots
 * version is compiled in.
 */
typedef struct _GowlCaptureSource GowlCaptureSource;

struct _GowlCaptureSource {
	GowlCaptureSourceKind kind;
	gchar *id;
	gchar *title;
	gchar *app_id;
};

GType               gowl_capture_source_get_type (void) G_GNUC_CONST;

GowlCaptureSource * gowl_capture_source_new      (GowlCaptureSourceKind kind,
                                                  const gchar          *id,
                                                  const gchar          *title,
                                                  const gchar          *app_id);

GowlCaptureSource * gowl_capture_source_copy     (const GowlCaptureSource *self);

void                gowl_capture_source_free     (GowlCaptureSource    *self);

gboolean            gowl_capture_source_equals   (const GowlCaptureSource *a,
                                                  const GowlCaptureSource *b);

G_END_DECLS

#endif /* GOWL_CAPTURE_SOURCE_H */
