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
 * gowl-macro-private.h - The host side of the macro API.  Not installed.
 *
 * The macro API lives in libgowl because a macro .so resolves its calls
 * against the running compositor, and a module's symbols are not in
 * that scope.  What only the running macro module can do -- queue a
 * step on its timeline player, hop to the compositor thread, notify,
 * log -- is reached through the host vtable the module installs on each
 * context.  A context with no host (a unit test) runs steps directly.
 */

#ifndef GOWL_MACRO_PRIVATE_H
#define GOWL_MACRO_PRIVATE_H

#include "macro/gowl-macro.h"

G_BEGIN_DECLS

/**
 * GowlMacroStepKind:
 *
 * One queued input step.
 */
typedef enum {
	GOWL_MACRO_STEP_KEY,
	GOWL_MACRO_STEP_TEXT,
	GOWL_MACRO_STEP_BUTTON,
	GOWL_MACRO_STEP_WAIT,
	GOWL_MACRO_STEP_COMMAND,
	GOWL_MACRO_STEP_ACTION,
	GOWL_MACRO_STEP_FOCUS
} GowlMacroStepKind;

typedef struct {
	GowlMacroStepKind kind;
	GWeakRef          target;    /* GowlClient, empty = focused */
	gboolean          has_target;
	guint32           keysym;    /* KEY: resolved when played */
	guint32           keycode;   /* KEY: or a fixed keycode */
	guint32           modifiers;
	guint32           button;
	guint             ms;
	gchar            *text;      /* TEXT text, COMMAND line, ACTION arg */
	GowlAction        action;
} GowlMacroStep;

GowlMacroStep *gowl_macro_step_new      (GowlMacroStepKind kind);
void           gowl_macro_step_free     (GowlMacroStep    *step);
gboolean       gowl_macro_step_execute  (GowlCompositor   *compositor,
                                         GowlMacroStep    *step);

typedef void (*GowlMacroSyncFunc) (gpointer user_data);

/**
 * GowlMacroHost:
 *
 * What the macro module provides to a context.
 */
typedef struct {
	/* timeline mode: take ownership of @step; FALSE refuses (cap) */
	gboolean (*queue_step)    (GowlMacroContext *ctx, GowlMacroStep *step);
	/* threaded mode: run @func on the compositor thread and wait;
	   FALSE when the run was cancelled or the host is gone */
	gboolean (*invoke_sync)   (GowlMacroContext *ctx, GowlMacroSyncFunc func,
	                           gpointer data);
	void     (*notify)        (GowlMacroContext *ctx, const gchar *summary,
	                           const gchar *body);
	void     (*log)           (GowlMacroContext *ctx, const gchar *message);
} GowlMacroHost;

GowlMacroContext *gowl_macro_context_new          (GowlCompositor      *compositor,
                                                   const gchar         *name,
                                                   const gchar         *path,
                                                   const gchar * const *argv,
                                                   GowlMacroTrigger     trigger,
                                                   const gchar         *detail,
                                                   gboolean             threaded);
GowlMacroContext *gowl_macro_context_ref          (GowlMacroContext    *ctx);
void              gowl_macro_context_unref        (GowlMacroContext    *ctx);
void              gowl_macro_context_set_host     (GowlMacroContext    *ctx,
                                                   const GowlMacroHost *host,
                                                   gpointer             host_data);
gpointer          gowl_macro_context_get_host_data(GowlMacroContext    *ctx);
void              gowl_macro_context_cancel       (GowlMacroContext    *ctx);
guint             gowl_macro_context_get_timeout  (GowlMacroContext    *ctx);
void              gowl_macro_context_set_timeout_default (GowlMacroContext *ctx,
                                                   guint                timeout_ms);
const gchar      *gowl_macro_context_get_result   (GowlMacroContext    *ctx);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(GowlMacroContext, gowl_macro_context_unref)

/* The C-macro registry (gowl_macro_register_func) */
gboolean          gowl_macro_registry_lookup      (const gchar         *name,
                                                   GowlMacroFunc       *out_func,
                                                   gpointer            *out_data);
GStrv             gowl_macro_registry_names       (void);

G_END_DECLS

#endif /* GOWL_MACRO_PRIVATE_H */
