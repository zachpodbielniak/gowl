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
 * gowl-gabbrev-render.h - Turning a match into the text it types.
 *
 * A render resolves the match's variables one at a time, in order --
 * global variables the replacement names first, then the match's own,
 * each after whatever it `depends_on' -- and substitutes them into the
 * replacement.  It is asynchronous because variables are: a shell
 * command, a picker, a language model and Lisp all answer later.
 *
 * The renderer resolves the types that need nothing but GLib itself
 * (date, echo, random, env, uuid, global, match).  Every other type
 * goes to the HOST -- the module, or a test -- as a
 * #GowlGabbrevRequest; the host answers it however it likes, on any
 * thread, and the request's answer is POSTED back so that the render
 * only ever continues on its own thread.
 */

#ifndef GOWL_GABBREV_RENDER_H
#define GOWL_GABBREV_RENDER_H

#include <glib.h>

#include "gowl-gabbrev-model.h"
#include "gowl-gabbrev-engine.h"
#include "gabbrev/gowl-gabbrev-private.h"

G_BEGIN_DECLS

/* The deepest a `match' variable may nest */
#define GABBREV_MAX_RENDER_DEPTH (8)

/* The cursor hint: where the cursor ends up */
#define GABBREV_CURSOR_HINT "$|$"

typedef struct _GabbrevRender GabbrevRender;

typedef struct {
	/* A variable of a type the renderer does not resolve itself.
	   Answer @request, now or later.  FALSE: no such type. */
	gboolean (*resolve) (gpointer            host_data,
	                     GabbrevRender      *render,
	                     const GabbrevVar   *var,
	                     GowlGabbrevRequest *request);
	/* @request was answered, possibly on another thread: arrange for
	   gabbrev_render_deliver(@render, @request) on the render's thread.
	   Called with a reference to each, which the host releases after
	   delivering. */
	void     (*post)    (gpointer            host_data,
	                     GabbrevRender      *render,
	                     GowlGabbrevRequest *request);
} GabbrevRenderHost;

typedef void (*GabbrevRenderDone) (GabbrevRender *render,
                                   gpointer       user_data);

typedef struct {
	GabbrevSet       *set;
	GabbrevMatch     *match;
	GHashTable       *captures;    /* regex groups, may be NULL */
	const gchar      *typed;       /* the trigger as typed, may be NULL */
	GabbrevTypedCase  typed_case;
	const gchar      *app_id;
	const gchar      *title;
	guint             depth;       /* 0 for a match that was typed */
} GabbrevRenderInfo;

GabbrevRender *gabbrev_render_new         (const GabbrevRenderInfo  *info,
                                           const GabbrevRenderHost  *host,
                                           gpointer                  host_data,
                                           GabbrevRenderDone         done,
                                           gpointer                  user_data);
GabbrevRender *gabbrev_render_ref         (GabbrevRender            *render);
void           gabbrev_render_unref       (GabbrevRender            *render);
void           gabbrev_render_start       (GabbrevRender            *render);
void           gabbrev_render_deliver     (GabbrevRender            *render,
                                           GowlGabbrevRequest       *request);
void           gabbrev_render_cancel      (GabbrevRender            *render);

gboolean       gabbrev_render_is_done     (GabbrevRender            *render);
const gchar   *gabbrev_render_get_text    (GabbrevRender            *render);
const gchar   *gabbrev_render_get_html    (GabbrevRender            *render);
const gchar   *gabbrev_render_get_error   (GabbrevRender            *render);
gint           gabbrev_render_get_cursor_back (GabbrevRender        *render);
GHashTable    *gabbrev_render_get_vars    (GabbrevRender            *render);
GabbrevMatch  *gabbrev_render_get_match   (GabbrevRender            *render);
const gchar   *gabbrev_render_get_pending_type (GabbrevRender       *render);

/* Pure pieces, shared with the tests */
gchar         *gabbrev_substitute         (const gchar              *text,
                                           GHashTable               *vars,
                                           GPtrArray                *missing);
GPtrArray     *gabbrev_referenced_names   (const gchar              *text);
gchar         *gabbrev_format_date        (const gchar              *format,
                                           GDateTime                *when);
gchar         *gabbrev_take_cursor_hint   (const gchar              *text,
                                           gint                     *out_back);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(GabbrevRender, gabbrev_render_unref)

G_END_DECLS

#endif /* GOWL_GABBREV_RENDER_H */
