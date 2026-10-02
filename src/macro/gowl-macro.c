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
 * gowl-macro.c - The macro API: contexts, steps, helpers, the C-macro
 * registry.  The runtime around it (lookup, compiling, guarded runs,
 * the timeline player, workers, triggers) is the opt-in `macro' module.
 *
 * The one rule this file keeps: nothing a THREADED macro calls touches
 * compositor state from the worker.  Steps and helpers hop to the
 * compositor thread through the host's invoke_sync; a timeline-mode
 * macro is already there.
 */

#include "macro/gowl-macro.h"
#include "macro/gowl-macro-private.h"
#include "core/gowl-core-private.h"
#include "config/gowl-keybind.h"
#include "boxed/gowl-input-remap-rule.h"
#include "util/gowl-fault-guard.h"

#include <linux/input-event-codes.h>
#include <stdarg.h>
#include <string.h>

struct _GowlMacroContext {
	gint                 ref_count;
	GowlCompositor      *compositor;
	gchar               *name;
	gchar               *path;
	GStrv                argv;
	GowlMacroTrigger     trigger;
	gchar               *detail;
	gboolean             threaded;

	const GowlMacroHost *host;
	gpointer             host_data;

	gint                 cancelled;
	GMutex               lock;
	GCond                cond;
	gchar               *result;
	guint                timeout_ms;
};

/* --- GowlMacroTrigger --- */

/**
 * gowl_macro_trigger_get_type:
 *
 * Returns: the #GType for #GowlMacroTrigger
 */
GType
gowl_macro_trigger_get_type(void)
{
	static gsize type_id = 0;

	if (g_once_init_enter(&type_id)) {
		static const GEnumValue values[] = {
			{ GOWL_MACRO_TRIGGER_API,   "GOWL_MACRO_TRIGGER_API",   "api" },
			{ GOWL_MACRO_TRIGGER_IPC,   "GOWL_MACRO_TRIGGER_IPC",   "ipc" },
			{ GOWL_MACRO_TRIGGER_DBUS,  "GOWL_MACRO_TRIGGER_DBUS",  "dbus" },
			{ GOWL_MACRO_TRIGGER_EVENT, "GOWL_MACRO_TRIGGER_EVENT", "event" },
			{ GOWL_MACRO_TRIGGER_TIMER, "GOWL_MACRO_TRIGGER_TIMER", "timer" },
			{ GOWL_MACRO_TRIGGER_REMAP, "GOWL_MACRO_TRIGGER_REMAP", "remap" },
			{ GOWL_MACRO_TRIGGER_VOICE, "GOWL_MACRO_TRIGGER_VOICE", "voice" },
			{ 0, NULL, NULL }
		};
		GType t = g_enum_register_static("GowlMacroTrigger", values);
		g_once_init_leave(&type_id, t);
	}
	return (GType)type_id;
}

/* --- contexts --- */

/**
 * gowl_macro_context_new: (skip)
 * @compositor: the compositor
 * @name: the macro's name
 * @path: (nullable): the file it came from
 * @argv: (nullable): its arguments
 * @trigger: what started it
 * @detail: (nullable): more about the trigger
 * @threaded: whether it runs on a worker
 *
 * Returns: (transfer full): a new context, with no host
 */
GowlMacroContext *
gowl_macro_context_new(
	GowlCompositor      *compositor,
	const gchar         *name,
	const gchar         *path,
	const gchar * const *argv,
	GowlMacroTrigger     trigger,
	const gchar         *detail,
	gboolean             threaded
){
	GowlMacroContext *ctx;

	ctx = g_new0(GowlMacroContext, 1);
	ctx->ref_count = 1;
	ctx->compositor = compositor;
	ctx->name = g_strdup(name != NULL ? name : "");
	ctx->path = g_strdup(path);
	ctx->argv = argv != NULL ? g_strdupv((gchar **)argv) : g_new0(gchar *, 1);
	ctx->trigger = trigger;
	ctx->detail = g_strdup(detail != NULL ? detail : "");
	ctx->threaded = threaded;
	g_mutex_init(&ctx->lock);
	g_cond_init(&ctx->cond);
	return ctx;
}

/**
 * gowl_macro_context_ref: (skip)
 * @ctx: a context
 *
 * Returns: (transfer full): @ctx
 */
GowlMacroContext *
gowl_macro_context_ref(GowlMacroContext *ctx)
{
	g_return_val_if_fail(ctx != NULL, NULL);
	g_atomic_int_inc(&ctx->ref_count);
	return ctx;
}

/**
 * gowl_macro_context_unref: (skip)
 * @ctx: (nullable): a context
 */
void
gowl_macro_context_unref(GowlMacroContext *ctx)
{
	if (ctx == NULL || !g_atomic_int_dec_and_test(&ctx->ref_count))
		return;
	g_free(ctx->name);
	g_free(ctx->path);
	g_strfreev(ctx->argv);
	g_free(ctx->detail);
	g_free(ctx->result);
	g_mutex_clear(&ctx->lock);
	g_cond_clear(&ctx->cond);
	g_free(ctx);
}

/**
 * gowl_macro_context_set_host: (skip)
 * @ctx: a context
 * @host: (nullable): the module's vtable
 * @host_data: (nullable): the module's per-run data
 */
void
gowl_macro_context_set_host(
	GowlMacroContext    *ctx,
	const GowlMacroHost *host,
	gpointer             host_data
){
	g_return_if_fail(ctx != NULL);
	ctx->host = host;
	ctx->host_data = host_data;
}

/**
 * gowl_macro_context_get_host_data: (skip)
 * @ctx: a context
 *
 * Returns: (transfer none) (nullable): the host's per-run data
 */
gpointer
gowl_macro_context_get_host_data(GowlMacroContext *ctx)
{
	g_return_val_if_fail(ctx != NULL, NULL);
	return ctx->host_data;
}

/**
 * gowl_macro_context_cancel: (skip)
 * @ctx: a context
 *
 * Marks the run cancelled and wakes a sleeping threaded macro.
 */
void
gowl_macro_context_cancel(GowlMacroContext *ctx)
{
	g_return_if_fail(ctx != NULL);
	gowl_fault_guard_hold();
	g_mutex_lock(&ctx->lock);
	g_atomic_int_set(&ctx->cancelled, 1);
	g_cond_broadcast(&ctx->cond);
	g_mutex_unlock(&ctx->lock);
	gowl_fault_guard_release();
}

/**
 * gowl_macro_context_get_timeout: (skip)
 * @ctx: a context
 *
 * Returns: the budget in force, as last set, in milliseconds
 */
guint
gowl_macro_context_get_timeout(GowlMacroContext *ctx)
{
	g_return_val_if_fail(ctx != NULL, 0);
	return ctx->timeout_ms;
}

/**
 * gowl_macro_context_set_timeout_default: (skip)
 * @ctx: a context
 * @timeout_ms: the budget the run starts with
 */
void
gowl_macro_context_set_timeout_default(
	GowlMacroContext *ctx,
	guint             timeout_ms
){
	g_return_if_fail(ctx != NULL);
	ctx->timeout_ms = timeout_ms;
}

/**
 * gowl_macro_context_get_result: (skip)
 * @ctx: a context
 *
 * Returns: (transfer none) (nullable): what gowl_macro_set_result() set
 */
const gchar *
gowl_macro_context_get_result(GowlMacroContext *ctx)
{
	g_return_val_if_fail(ctx != NULL, NULL);
	return ctx->result;
}

/* --- the run, for macros --- */

/**
 * gowl_macro_get_compositor:
 * @ctx: the run
 *
 * In a threaded macro, touch the compositor only from inside
 * gowl_macro_call_on_compositor().
 *
 * Returns: (transfer none): the compositor
 */
GowlCompositor *
gowl_macro_get_compositor(GowlMacroContext *ctx)
{
	g_return_val_if_fail(ctx != NULL, NULL);
	return ctx->compositor;
}

/**
 * gowl_macro_get_name:
 * @ctx: the run
 *
 * Returns: (transfer none): the name the macro was run by
 */
const gchar *
gowl_macro_get_name(GowlMacroContext *ctx)
{
	g_return_val_if_fail(ctx != NULL, NULL);
	return ctx->name;
}

/**
 * gowl_macro_get_path:
 * @ctx: the run
 *
 * Returns: (transfer none) (nullable): the source file, %NULL for a
 *   macro registered from C or defined over IPC
 */
const gchar *
gowl_macro_get_path(GowlMacroContext *ctx)
{
	g_return_val_if_fail(ctx != NULL, NULL);
	return ctx->path;
}

/**
 * gowl_macro_get_argc:
 * @ctx: the run
 *
 * Returns: how many arguments the macro was given
 */
guint
gowl_macro_get_argc(GowlMacroContext *ctx)
{
	g_return_val_if_fail(ctx != NULL, 0);
	return g_strv_length(ctx->argv);
}

/**
 * gowl_macro_get_arg:
 * @ctx: the run
 * @index: 0-based
 *
 * Returns: (transfer none) (nullable): argument @index, or %NULL
 */
const gchar *
gowl_macro_get_arg(
	GowlMacroContext *ctx,
	guint             index
){
	g_return_val_if_fail(ctx != NULL, NULL);
	return index < g_strv_length(ctx->argv) ? ctx->argv[index] : NULL;
}

/**
 * gowl_macro_get_argv:
 * @ctx: the run
 *
 * Returns: (transfer none) (array zero-terminated=1): every argument
 */
const gchar * const *
gowl_macro_get_argv(GowlMacroContext *ctx)
{
	g_return_val_if_fail(ctx != NULL, NULL);
	return (const gchar * const *)ctx->argv;
}

/**
 * gowl_macro_get_trigger:
 * @ctx: the run
 *
 * Returns: what started the run
 */
GowlMacroTrigger
gowl_macro_get_trigger(GowlMacroContext *ctx)
{
	g_return_val_if_fail(ctx != NULL, GOWL_MACRO_TRIGGER_API);
	return ctx->trigger;
}

/**
 * gowl_macro_get_trigger_detail:
 * @ctx: the run
 *
 * Returns: (transfer none): the event name, the remap rule and input,
 *   the command line -- whatever says more about the trigger
 */
const gchar *
gowl_macro_get_trigger_detail(GowlMacroContext *ctx)
{
	g_return_val_if_fail(ctx != NULL, NULL);
	return ctx->detail;
}

/**
 * gowl_macro_is_threaded:
 * @ctx: the run
 *
 * Returns: %TRUE on a worker thread
 */
gboolean
gowl_macro_is_threaded(GowlMacroContext *ctx)
{
	g_return_val_if_fail(ctx != NULL, FALSE);
	return ctx->threaded;
}

/* --- control --- */

/**
 * gowl_macro_set_timeout:
 * @ctx: the run
 * @timeout_ms: the budget from now; 0 switches the watchdog off
 *
 * Grants the running macro more (or less) time.  Call it first thing
 * in a macro that knows it is long.  In a threaded macro, time spent in
 * gowl_macro_sleep() counts.
 *
 * Returns: %TRUE when a guarded run was extended
 */
gboolean
gowl_macro_set_timeout(
	GowlMacroContext *ctx,
	guint             timeout_ms
){
	g_return_val_if_fail(ctx != NULL, FALSE);
	ctx->timeout_ms = timeout_ms;
	return gowl_fault_guard_extend(timeout_ms);
}

/**
 * gowl_macro_is_cancelled:
 * @ctx: the run
 *
 * Returns: %TRUE once `macro-stop' or Super+Escape asked the run to end;
 *   a long macro should check it and return
 */
gboolean
gowl_macro_is_cancelled(GowlMacroContext *ctx)
{
	g_return_val_if_fail(ctx != NULL, TRUE);
	return g_atomic_int_get(&ctx->cancelled) != 0;
}

/**
 * gowl_macro_sleep:
 * @ctx: the run
 * @ms: how long
 *
 * Sleeps, waking early when cancelled.  Threaded macros only: in the
 * default mode the macro runs on the compositor thread, where a sleep
 * freezes the desktop, so this refuses and logs -- use
 * gowl_macro_wait(), which queues the pause instead.
 *
 * Returns: %TRUE after the full sleep, %FALSE when cancelled (return
 *   from the macro) or refused
 */
gboolean
gowl_macro_sleep(
	GowlMacroContext *ctx,
	guint             ms
){
	gint64 end;

	g_return_val_if_fail(ctx != NULL, FALSE);
	if (!ctx->threaded) {
		gowl_macro_log(ctx, "gowl_macro_sleep() in a macro that is not "
		               "threaded would freeze the desktop; use "
		               "gowl_macro_wait() or #define GOWL_MACRO_THREADED 1");
		return FALSE;
	}

	/* Waited in slices with the watchdog held off only while the lock
	   is ours: a budget that runs out mid-sleep unwinds between slices,
	   never with ctx->lock taken (the compositor's cancel needs it). */
	end = g_get_monotonic_time() + (gint64)ms * G_TIME_SPAN_MILLISECOND;
	while (!g_atomic_int_get(&ctx->cancelled)
	       && g_get_monotonic_time() < end) {
		gint64 slice = MIN(end, g_get_monotonic_time()
		                        + 50 * G_TIME_SPAN_MILLISECOND);

		gowl_fault_guard_hold();
		g_mutex_lock(&ctx->lock);
		if (!g_atomic_int_get(&ctx->cancelled))
			g_cond_wait_until(&ctx->cond, &ctx->lock, slice);
		g_mutex_unlock(&ctx->lock);
		gowl_fault_guard_release();
	}
	return !g_atomic_int_get(&ctx->cancelled);
}

/**
 * gowl_macro_log:
 * @ctx: the run
 * @format: printf-style format
 * @...: arguments
 *
 * A line in the macro module's log (stderr, or its `log-file'),
 * prefixed with the macro's name.
 */
void
gowl_macro_log(
	GowlMacroContext *ctx,
	const gchar      *format,
	...
){
	g_autofree gchar *msg = NULL;
	va_list ap;

	g_return_if_fail(ctx != NULL);
	va_start(ap, format);
	msg = g_strdup_vprintf(format, ap);
	va_end(ap);
	if (ctx->host != NULL && ctx->host->log != NULL)
		ctx->host->log(ctx, msg);
	else
		g_message("macro %s: %s", ctx->name, msg);
}

/**
 * gowl_macro_notify:
 * @ctx: the run
 * @summary: the headline
 * @body: (nullable): more
 *
 * A user-visible notification: a bar toast when the bar is loaded, and
 * the compositor's toast signal (layout-indicator, embedders).
 */
void
gowl_macro_notify(
	GowlMacroContext *ctx,
	const gchar      *summary,
	const gchar      *body
){
	g_return_if_fail(ctx != NULL);
	g_return_if_fail(summary != NULL);
	if (ctx->host != NULL && ctx->host->notify != NULL)
		ctx->host->notify(ctx, summary, body);
	else
		g_message("macro %s: %s%s%s", ctx->name, summary,
		          body != NULL ? ": " : "", body != NULL ? body : "");
}

/**
 * gowl_macro_set_result:
 * @ctx: the run
 * @text: (nullable): a one-line result
 *
 * What `macro-run' replies with, after "OK ", for a macro in the
 * default mode.  A threaded macro's result is in `macro-status' and the
 * finished event.
 */
void
gowl_macro_set_result(
	GowlMacroContext *ctx,
	const gchar      *text
){
	gchar *copy;
	gchar *old;

	g_return_if_fail(ctx != NULL);
	/* Copied before the lock: a bad @text faults outside it */
	copy = g_strdup(text);
	gowl_fault_guard_hold();
	g_mutex_lock(&ctx->lock);
	old = ctx->result;
	ctx->result = copy;
	g_mutex_unlock(&ctx->lock);
	gowl_fault_guard_release();
	g_free(old);
}

typedef struct {
	GowlMacroCompositorFunc func;
	GowlCompositor         *compositor;
	gpointer                data;
} CallOn;

static void
call_on_trampoline(gpointer data)
{
	CallOn *c = data;

	c->func(c->compositor, c->data);
}

/**
 * gowl_macro_call_on_compositor:
 * @ctx: the run
 * @func: (scope call): code that touches compositor state
 * @user_data: passed to @func
 *
 * Runs @func on the compositor thread and waits for it.  A macro in the
 * default mode is already there, so @func just runs.  A threaded macro
 * MUST use this for anything beyond the gowl_macro_* functions (which
 * hop by themselves).
 *
 * Returns: %TRUE when @func ran; %FALSE when the run was cancelled
 */
gboolean
gowl_macro_call_on_compositor(
	GowlMacroContext        *ctx,
	GowlMacroCompositorFunc  func,
	gpointer                 user_data
){
	CallOn c;

	g_return_val_if_fail(ctx != NULL, FALSE);
	g_return_val_if_fail(func != NULL, FALSE);

	c.func = func;
	c.compositor = ctx->compositor;
	c.data = user_data;
	if (!ctx->threaded || ctx->host == NULL || ctx->host->invoke_sync == NULL) {
		call_on_trampoline(&c);
		return TRUE;
	}
	if (gowl_macro_is_cancelled(ctx))
		return FALSE;
	return ctx->host->invoke_sync(ctx, call_on_trampoline, &c);
}

/* --- steps --- */

/**
 * gowl_macro_step_new: (skip)
 * @kind: what the step does
 *
 * Returns: (transfer full): a new, empty step
 */
GowlMacroStep *
gowl_macro_step_new(GowlMacroStepKind kind)
{
	GowlMacroStep *step;

	step = g_new0(GowlMacroStep, 1);
	step->kind = kind;
	g_weak_ref_init(&step->target, NULL);
	return step;
}

/**
 * gowl_macro_step_free: (skip)
 * @step: (nullable): a step
 */
void
gowl_macro_step_free(GowlMacroStep *step)
{
	if (step == NULL)
		return;
	g_weak_ref_clear(&step->target);
	g_free(step->text);
	g_free(step);
}

static void
step_set_target(
	GowlMacroStep *step,
	GowlClient    *target
){
	if (target == NULL)
		return;
	g_weak_ref_set(&step->target, target);
	step->has_target = TRUE;
}

/* Presses (or releases, in reverse) the modifier keys @mods names, on
   the real key path, for a key sent to the focused window. */
static void
focused_modifiers(
	GowlCompositor *comp,
	guint32         mods,
	gboolean        pressed
){
	static const struct {
		guint32 bit;
		guint32 keysym;
	} table[] = {
		{ GOWL_KEY_MOD_LOGO,  XKB_KEY_Super_L          },
		{ GOWL_KEY_MOD_CTRL,  XKB_KEY_Control_L        },
		{ GOWL_KEY_MOD_ALT,   XKB_KEY_Alt_L            },
		{ GOWL_KEY_MOD_SHIFT, XKB_KEY_Shift_L          },
		{ GOWL_KEY_MOD_MOD5,  XKB_KEY_ISO_Level3_Shift },
	};
	guint i;

	for (i = 0; i < G_N_ELEMENTS(table); i++) {
		guint idx = pressed ? i : (guint)G_N_ELEMENTS(table) - 1 - i;
		guint32 kc;

		if ((mods & table[idx].bit) == 0)
			continue;
		kc = gowl_compositor_keysym_to_keycode(comp, table[idx].keysym, NULL);
		if (kc != 0)
			gowl_compositor_inject_key(comp, kc, pressed);
	}
}

/**
 * gowl_macro_step_execute: (skip)
 * @compositor: the compositor, on its own thread
 * @step: the step
 *
 * Performs one step.  A step aimed at a window that has gone is
 * skipped; nothing but a wait runs while the session is locked.
 *
 * Returns: %TRUE when the step did something
 */
gboolean
gowl_macro_step_execute(
	GowlCompositor *compositor,
	GowlMacroStep  *step
){
	g_autoptr(GowlClient) target = NULL;
	gboolean ok;

	g_return_val_if_fail(GOWL_IS_COMPOSITOR(compositor), FALSE);
	g_return_val_if_fail(step != NULL, FALSE);

	if (step->kind == GOWL_MACRO_STEP_WAIT)
		return TRUE;
	if (compositor->locked)
		return FALSE;

	if (step->has_target) {
		target = (GowlClient *)g_weak_ref_get(&step->target);
		if (target == NULL
		    || g_list_find(gowl_compositor_get_clients(compositor),
		                   target) == NULL)
			return FALSE;
	}

	ok = TRUE;
	switch (step->kind) {
	case GOWL_MACRO_STEP_KEY: {
		guint32 keycode;
		guint32 mods;
		gboolean shift;

		shift = FALSE;
		keycode = step->keycode != 0
			? step->keycode
			: gowl_compositor_keysym_to_keycode(compositor, step->keysym,
			                                    &shift);
		if (keycode == 0)
			return FALSE;
		mods = step->modifiers | (shift ? (guint32)GOWL_KEY_MOD_SHIFT : 0);
		if (target != NULL) {
			ok = gowl_compositor_send_key_to_client(compositor, target,
			                                        keycode, mods);
		} else {
			/* The focused window, through the whole key decision:
			   a macro's Super+9 is the tag-9 keybind. */
			focused_modifiers(compositor, mods, TRUE);
			gowl_compositor_inject_key(compositor, keycode, TRUE);
			gowl_compositor_inject_key(compositor, keycode, FALSE);
			focused_modifiers(compositor, mods, FALSE);
		}
		break;
	}
	case GOWL_MACRO_STEP_TEXT: {
		GowlClient *into;

		into = target != NULL ? target
		                      : gowl_compositor_get_focused_client(compositor);
		ok = into != NULL
		     && gowl_compositor_send_text_to_client(compositor, into,
		                                            step->text) >= 0;
		break;
	}
	case GOWL_MACRO_STEP_BUTTON:
		gowl_compositor_inject_button(compositor, step->button, TRUE);
		gowl_compositor_inject_button(compositor, step->button, FALSE);
		break;
	case GOWL_MACRO_STEP_BUTTON_STATE:
		gowl_compositor_inject_button(compositor, step->button,
		                              step->pressed);
		break;
	case GOWL_MACRO_STEP_POINTER:
		/* Motion, not a warp: the client under the pointer is told it
		   moved, which is what a hover or a drag in a recording needs.
		   It also obeys a pointer lock, as a hand on a mouse would. */
		gowl_compositor_inject_pointer_warp(compositor, step->x, step->y);
		break;
	case GOWL_MACRO_STEP_SCROLL:
		gowl_compositor_inject_axis(compositor, step->horizontal,
		                            step->value, step->discrete);
		break;
	case GOWL_MACRO_STEP_COMMAND:
		g_free(gowl_compositor_run_command(compositor, step->text));
		break;
	case GOWL_MACRO_STEP_ACTION: {
		GowlKeybindEntry kb;

		memset(&kb, 0, sizeof kb);
		kb.action = (gint)step->action;
		kb.arg = step->text;
		ok = gowl_compositor_run_keybind_entry(compositor, &kb);
		break;
	}
	case GOWL_MACRO_STEP_FOCUS:
		if (target != NULL)
			gowl_compositor_show_client(compositor, target);
		else
			ok = FALSE;
		break;
	case GOWL_MACRO_STEP_WAIT:
	default:
		break;
	}
	return ok;
}

typedef struct {
	GowlCompositor *compositor;
	GowlMacroStep  *step;
	gboolean        ok;
} StepCall;

static void
step_call_trampoline(gpointer data)
{
	StepCall *c = data;

	c->ok = gowl_macro_step_execute(c->compositor, c->step);
}

/*
 * One step, the way the context runs them: queued on the module's
 * timeline in the default mode, performed now (on the compositor
 * thread) in a threaded macro.  Takes ownership of @step.
 */
static gboolean
submit(
	GowlMacroContext *ctx,
	GowlMacroStep    *step
){
	gboolean ok;

	if (gowl_macro_is_cancelled(ctx)) {
		gowl_macro_step_free(step);
		return FALSE;
	}

	if (!ctx->threaded) {
		if (ctx->host != NULL && ctx->host->queue_step != NULL)
			return ctx->host->queue_step(ctx, step);
		/* No host (a test): perform it now; waits are no-ops. */
		ok = gowl_macro_step_execute(ctx->compositor, step);
		gowl_macro_step_free(step);
		return ok;
	}

	if (step->kind == GOWL_MACRO_STEP_WAIT) {
		ok = gowl_macro_sleep(ctx, step->ms);
		gowl_macro_step_free(step);
		return ok;
	}

	{
		StepCall c;

		c.compositor = ctx->compositor;
		c.step = step;
		c.ok = FALSE;
		if (ctx->host != NULL && ctx->host->invoke_sync != NULL)
			ok = ctx->host->invoke_sync(ctx, step_call_trampoline, &c)
			     && c.ok;
		else {
			step_call_trampoline(&c);
			ok = c.ok;
		}
	}
	gowl_macro_step_free(step);
	return ok;
}

/**
 * gowl_macro_key:
 * @ctx: the run
 * @target: (nullable): the window to type into; %NULL for the focused
 *   one, through the compositor's keybinds (a macro's `Super+9' views
 *   tag 9)
 * @combo: modifiers and ONE key: "Super+9", "ctrl+shift+t", "Return",
 *   "KEY_F13"
 *
 * One key press and release.
 *
 * Returns: %TRUE when queued (or, threaded, performed)
 */
gboolean
gowl_macro_key(
	GowlMacroContext *ctx,
	GowlClient       *target,
	const gchar      *combo
){
	GowlMacroStep *step;
	g_auto(GStrv) parts = NULL;
	guint n;

	g_return_val_if_fail(ctx != NULL, FALSE);
	g_return_val_if_fail(combo != NULL, FALSE);

	step = gowl_macro_step_new(GOWL_MACRO_STEP_KEY);
	parts = g_strsplit(combo, "+", -1);
	n = g_strv_length(parts);

	if (n > 0 && g_str_has_prefix(g_strstrip(parts[n - 1]), "KEY_")) {
		/* An evdev key: the modifiers parse with a stand-in key. */
		guint32 code;
		g_autofree gchar *mods_only = NULL;
		guint keysym;

		if (!gowl_input_remap_code_from_name(parts[n - 1], &code)
		    || code > KEY_MAX) {
			gowl_macro_log(ctx, "not a key: %s", combo);
			gowl_macro_step_free(step);
			return FALSE;
		}
		g_free(parts[n - 1]);
		parts[n - 1] = g_strdup("a");
		mods_only = g_strjoinv("+", parts);
		if (!gowl_keybind_parse(mods_only, &step->modifiers, &keysym)) {
			gowl_macro_step_free(step);
			return FALSE;
		}
		step->keycode = code;
	} else if (!gowl_keybind_parse(combo, &step->modifiers, &step->keysym)) {
		gowl_macro_log(ctx, "not a key combination: %s", combo);
		gowl_macro_step_free(step);
		return FALSE;
	}
	step_set_target(step, target);
	return submit(ctx, step);
}

/**
 * gowl_macro_key_code:
 * @ctx: the run
 * @target: (nullable): the window, %NULL for the focused one
 * @keycode: an evdev keycode (KEY_*)
 * @modifiers: #GowlKeyMod bits held for it
 *
 * Returns: %TRUE when queued (or performed)
 */
gboolean
gowl_macro_key_code(
	GowlMacroContext *ctx,
	GowlClient       *target,
	guint32           keycode,
	guint32           modifiers
){
	GowlMacroStep *step;

	g_return_val_if_fail(ctx != NULL, FALSE);
	g_return_val_if_fail(keycode > 0 && keycode <= KEY_MAX, FALSE);

	step = gowl_macro_step_new(GOWL_MACRO_STEP_KEY);
	step->keycode = keycode;
	step->modifiers = modifiers;
	step_set_target(step, target);
	return submit(ctx, step);
}

/**
 * gowl_macro_text:
 * @ctx: the run
 * @target: (nullable): the window, %NULL for the focused one
 * @text: UTF-8 text; `\n' is Return
 *
 * Types @text into @target without moving focus for good.
 *
 * Returns: %TRUE when queued (or performed)
 */
gboolean
gowl_macro_text(
	GowlMacroContext *ctx,
	GowlClient       *target,
	const gchar      *text
){
	GowlMacroStep *step;

	g_return_val_if_fail(ctx != NULL, FALSE);
	g_return_val_if_fail(text != NULL, FALSE);

	step = gowl_macro_step_new(GOWL_MACRO_STEP_TEXT);
	step->text = g_strdup(text);
	step_set_target(step, target);
	return submit(ctx, step);
}

/**
 * gowl_macro_button:
 * @ctx: the run
 * @button: a BTN_* code
 *
 * One click where the cursor is, through the compositor's button path.
 *
 * Returns: %TRUE when queued (or performed)
 */
gboolean
gowl_macro_button(
	GowlMacroContext *ctx,
	guint32           button
){
	GowlMacroStep *step;

	g_return_val_if_fail(ctx != NULL, FALSE);
	step = gowl_macro_step_new(GOWL_MACRO_STEP_BUTTON);
	step->button = button;
	return submit(ctx, step);
}

/**
 * gowl_macro_button_state:
 * @ctx: the run
 * @button: a BTN_* code
 * @pressed: %TRUE for down, %FALSE for up
 *
 * Half a click: the press or the release alone.  With
 * gowl_macro_pointer() between them it is a drag -- what the macro
 * recorder writes for one.  A macro that presses and never releases
 * leaves the button held, so pair them.
 *
 * Returns: %TRUE when queued (or performed)
 */
gboolean
gowl_macro_button_state(
	GowlMacroContext *ctx,
	guint32           button,
	gboolean          pressed
){
	GowlMacroStep *step;

	g_return_val_if_fail(ctx != NULL, FALSE);
	step = gowl_macro_step_new(GOWL_MACRO_STEP_BUTTON_STATE);
	step->button = button;
	step->pressed = pressed;
	return submit(ctx, step);
}

/**
 * gowl_macro_pointer:
 * @ctx: the run
 * @x: layout x, the same coordinates `gowl-msg clients' reports
 * @y: layout y
 *
 * Moves the pointer there.  It is pointer motion, so the window under
 * it hears about it (hover, a drag in progress); a point off every
 * output is clamped to the nearest one.
 *
 * Returns: %TRUE when queued (or performed)
 */
gboolean
gowl_macro_pointer(
	GowlMacroContext *ctx,
	gdouble           x,
	gdouble           y
){
	GowlMacroStep *step;

	g_return_val_if_fail(ctx != NULL, FALSE);
	step = gowl_macro_step_new(GOWL_MACRO_STEP_POINTER);
	step->x = x;
	step->y = y;
	return submit(ctx, step);
}

/**
 * gowl_macro_scroll:
 * @ctx: the run
 * @horizontal: %TRUE for sideways
 * @value: the delta, in surface units (15 is one notch on most mice)
 * @discrete: the wheel amount in 120ths of a notch (120 = one notch),
 *   or 0 for a smooth, touchpad-like scroll
 *
 * One scroll where the pointer is.
 *
 * Returns: %TRUE when queued (or performed)
 */
gboolean
gowl_macro_scroll(
	GowlMacroContext *ctx,
	gboolean          horizontal,
	gdouble           value,
	gint              discrete
){
	GowlMacroStep *step;

	g_return_val_if_fail(ctx != NULL, FALSE);
	step = gowl_macro_step_new(GOWL_MACRO_STEP_SCROLL);
	step->horizontal = horizontal;
	step->value = value;
	step->discrete = discrete;
	return submit(ctx, step);
}

/**
 * gowl_macro_wait:
 * @ctx: the run
 * @ms: how long
 *
 * A pause between steps.  Queued in the default mode (the desktop keeps
 * running); a real, cancellable sleep in a threaded macro.
 *
 * Returns: %TRUE when queued (or slept through)
 */
gboolean
gowl_macro_wait(
	GowlMacroContext *ctx,
	guint             ms
){
	GowlMacroStep *step;

	g_return_val_if_fail(ctx != NULL, FALSE);
	step = gowl_macro_step_new(GOWL_MACRO_STEP_WAIT);
	step->ms = ms;
	return submit(ctx, step);
}

/**
 * gowl_macro_command:
 * @ctx: the run
 * @line: a command line for the modules (`scratchpad-toggle',
 *   `bar-notify ...', another `macro-run ...')
 *
 * Returns: %TRUE when queued (or performed)
 */
gboolean
gowl_macro_command(
	GowlMacroContext *ctx,
	const gchar      *line
){
	GowlMacroStep *step;

	g_return_val_if_fail(ctx != NULL, FALSE);
	g_return_val_if_fail(line != NULL, FALSE);
	step = gowl_macro_step_new(GOWL_MACRO_STEP_COMMAND);
	step->text = g_strdup(line);
	return submit(ctx, step);
}

/**
 * gowl_macro_action:
 * @ctx: the run
 * @action: a compositor action
 * @arg: (nullable): its argument
 *
 * Returns: %TRUE when queued (or performed)
 */
gboolean
gowl_macro_action(
	GowlMacroContext *ctx,
	GowlAction        action,
	const gchar      *arg
){
	GowlMacroStep *step;

	g_return_val_if_fail(ctx != NULL, FALSE);
	step = gowl_macro_step_new(GOWL_MACRO_STEP_ACTION);
	step->action = action;
	step->text = g_strdup(arg);
	return submit(ctx, step);
}

/**
 * gowl_macro_focus:
 * @ctx: the run
 * @client: the window to jump to (its tags viewed, focused)
 *
 * Returns: %TRUE when queued (or performed)
 */
gboolean
gowl_macro_focus(
	GowlMacroContext *ctx,
	GowlClient       *client
){
	GowlMacroStep *step;

	g_return_val_if_fail(ctx != NULL, FALSE);
	if (client == NULL)
		return FALSE;
	step = gowl_macro_step_new(GOWL_MACRO_STEP_FOCUS);
	step_set_target(step, client);
	return submit(ctx, step);
}

/* --- compositor helpers --- */

typedef struct {
	const gchar *pattern;
	GowlClient  *found;
} FindCall;

static void
find_client_on(
	GowlCompositor *comp,
	gpointer        data
){
	FindCall *f = data;

	if (g_str_has_prefix(f->pattern, "title:"))
		f->found = gowl_compositor_find_client_by_title(
			comp, f->pattern + strlen("title:"));
	else
		f->found = gowl_compositor_find_client_by_app_id(
			comp, g_str_has_prefix(f->pattern, "app-id:")
			      ? f->pattern + strlen("app-id:") : f->pattern);
}

/**
 * gowl_macro_find_client:
 * @ctx: the run
 * @pattern: "app-id:GLOB", "title:GLOB", or a bare app-id glob
 *
 * Returns: (transfer none) (nullable): the first matching window
 */
GowlClient *
gowl_macro_find_client(
	GowlMacroContext *ctx,
	const gchar      *pattern
){
	FindCall f;

	g_return_val_if_fail(ctx != NULL, NULL);
	g_return_val_if_fail(pattern != NULL, NULL);
	f.pattern = pattern;
	f.found = NULL;
	gowl_macro_call_on_compositor(ctx, find_client_on, &f);
	return f.found;
}

typedef struct {
	gboolean visible_only;
	GList   *out;
} ListCall;

static void
list_clients_on(
	GowlCompositor *comp,
	gpointer        data
){
	ListCall *lc = data;
	GList *l;

	for (l = gowl_compositor_get_clients(comp); l != NULL; l = l->next) {
		GowlClient *c = l->data;

		if (lc->visible_only) {
			GowlMonitor *m = (GowlMonitor *)gowl_client_get_monitor(c);

			if (m == NULL
			    || (gowl_client_get_tags(c) & gowl_monitor_get_tags(m)) == 0)
				continue;
		}
		lc->out = g_list_prepend(lc->out, c);
	}
	lc->out = g_list_reverse(lc->out);
}

/**
 * gowl_macro_list_clients:
 * @ctx: the run
 * @visible_only: only windows on a tag their monitor shows
 *
 * The windows in tiling order.
 *
 * Returns: (transfer container) (element-type GowlClient): free with
 *   g_list_free()
 */
GList *
gowl_macro_list_clients(
	GowlMacroContext *ctx,
	gboolean          visible_only
){
	ListCall lc;

	g_return_val_if_fail(ctx != NULL, NULL);
	lc.visible_only = visible_only;
	lc.out = NULL;
	gowl_macro_call_on_compositor(ctx, list_clients_on, &lc);
	return lc.out;
}

typedef struct {
	GList        *clients;
	GCompareFunc  compare;
} SortCall;

static void
sort_clients_on(
	GowlCompositor *comp,
	gpointer        data
){
	SortCall *s = data;
	GList *sorted;

	sorted = g_list_sort(g_list_copy(s->clients), s->compare);
	gowl_compositor_reorder_clients(comp, sorted);
	g_list_free(sorted);
}

/**
 * gowl_macro_sort_clients:
 * @ctx: the run
 * @clients: (element-type GowlClient): the windows to put in order
 * @compare: (scope call): orders two #GowlClient pointers
 *
 * Sorts @clients with @compare and applies that order to the tiling,
 * in the slots they occupy.  Everything else stays where it is.
 */
void
gowl_macro_sort_clients(
	GowlMacroContext *ctx,
	GList            *clients,
	GCompareFunc      compare
){
	SortCall s;

	g_return_if_fail(ctx != NULL);
	g_return_if_fail(compare != NULL);
	s.clients = clients;
	s.compare = compare;
	gowl_macro_call_on_compositor(ctx, sort_clients_on, &s);
}

typedef struct {
	const gchar *line;
	gchar       *reply;
} CommandCall;

static void
command_on(
	GowlCompositor *comp,
	gpointer        data
){
	CommandCall *c = data;

	c->reply = gowl_compositor_run_command(comp, c->line);
}

/**
 * gowl_macro_run_command:
 * @ctx: the run
 * @line: a module command line
 *
 * Runs @line NOW and returns the reply (unlike gowl_macro_command(),
 * which is a step).
 *
 * Returns: (transfer full) (nullable): the reply, %NULL when no module
 *   answered
 */
gchar *
gowl_macro_run_command(
	GowlMacroContext *ctx,
	const gchar      *line
){
	CommandCall c;

	g_return_val_if_fail(ctx != NULL, NULL);
	g_return_val_if_fail(line != NULL, NULL);
	c.line = line;
	c.reply = NULL;
	gowl_macro_call_on_compositor(ctx, command_on, &c);
	return c.reply;
}

typedef struct {
	GowlClient  *client;
	GowlMonitor *monitor;
	guint32      tags;
} MoveCall;

static void
move_on(
	GowlCompositor *comp,
	gpointer        data
){
	MoveCall *m = data;

	if (g_list_find(gowl_compositor_get_clients(comp), m->client) != NULL)
		gowl_compositor_move_client(comp, m->client, m->monitor, m->tags);
}

/**
 * gowl_macro_move_client:
 * @ctx: the run
 * @client: the window
 * @monitor: (nullable): the output to move it to, %NULL to stay
 * @tags: the tags (a bitmask: tag N is 1 << (N-1)); 0 for the
 *   destination's current view, or to keep them when staying put
 *
 * Moves a window between outputs and tags, and re-tiles.
 */
void
gowl_macro_move_client(
	GowlMacroContext *ctx,
	GowlClient       *client,
	GowlMonitor      *monitor,
	guint32           tags
){
	MoveCall m;

	g_return_if_fail(ctx != NULL);
	if (client == NULL)
		return;
	m.client = client;
	m.monitor = monitor;
	m.tags = tags;
	gowl_macro_call_on_compositor(ctx, move_on, &m);
}

/* --- the C-macro registry --- */

typedef struct {
	GowlMacroFunc  func;
	gpointer       data;
	GDestroyNotify destroy;
} RegisteredMacro;

static GMutex      registry_lock;
static GHashTable *registry;

static void
registered_free(gpointer p)
{
	RegisteredMacro *r = p;

	if (r->destroy != NULL && r->data != NULL)
		r->destroy(r->data);
	g_free(r);
}

/**
 * gowl_macro_register_func:
 * @name: the name it runs by (`macro-run NAME')
 * @func: (scope notified) (closure user_data) (destroy destroy): the macro
 * @user_data: (nullable): passed to @func
 * @destroy: (nullable): frees @user_data
 *
 * A macro written in C -- in a C config, a module, an embedder.  It
 * shadows a file of the same name and runs under the same guard.  Can
 * be called before the macro module loads.
 */
void
gowl_macro_register_func(
	const gchar    *name,
	GowlMacroFunc   func,
	gpointer        user_data,
	GDestroyNotify  destroy
){
	RegisteredMacro *r;

	g_return_if_fail(name != NULL && *name != '\0');
	g_return_if_fail(func != NULL);

	r = g_new0(RegisteredMacro, 1);
	r->func = func;
	r->data = user_data;
	r->destroy = destroy;
	gowl_fault_guard_hold();
	g_mutex_lock(&registry_lock);
	if (registry == NULL)
		registry = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
		                                 registered_free);
	g_hash_table_replace(registry, g_strdup(name), r);
	g_mutex_unlock(&registry_lock);
	gowl_fault_guard_release();
}

/**
 * gowl_macro_unregister_func:
 * @name: a registered name
 *
 * Returns: %TRUE when a macro was removed
 */
gboolean
gowl_macro_unregister_func(const gchar *name)
{
	gboolean removed;

	g_return_val_if_fail(name != NULL, FALSE);
	gowl_fault_guard_hold();
	g_mutex_lock(&registry_lock);
	removed = registry != NULL && g_hash_table_remove(registry, name);
	g_mutex_unlock(&registry_lock);
	gowl_fault_guard_release();
	return removed;
}

/**
 * gowl_macro_registry_lookup: (skip)
 * @name: a name
 * @out_func: (out): the function
 * @out_data: (out): its data
 *
 * Returns: %TRUE when @name is registered
 */
gboolean
gowl_macro_registry_lookup(
	const gchar   *name,
	GowlMacroFunc *out_func,
	gpointer      *out_data
){
	RegisteredMacro *r;

	gowl_fault_guard_hold();
	g_mutex_lock(&registry_lock);
	r = registry != NULL ? g_hash_table_lookup(registry, name) : NULL;
	if (r != NULL) {
		*out_func = r->func;
		*out_data = r->data;
	}
	g_mutex_unlock(&registry_lock);
	gowl_fault_guard_release();
	return r != NULL;
}

/**
 * gowl_macro_registry_names: (skip)
 *
 * Returns: (transfer full): every registered name, sorted
 */
GStrv
gowl_macro_registry_names(void)
{
	GPtrArray *out;
	GHashTableIter iter;
	gpointer key;

	out = g_ptr_array_new();
	gowl_fault_guard_hold();
	g_mutex_lock(&registry_lock);
	if (registry != NULL) {
		g_hash_table_iter_init(&iter, registry);
		while (g_hash_table_iter_next(&iter, &key, NULL))
			g_ptr_array_add(out, g_strdup(key));
	}
	g_mutex_unlock(&registry_lock);
	gowl_fault_guard_release();
	g_ptr_array_sort_values(out, (GCompareFunc)g_ascii_strcasecmp);
	g_ptr_array_add(out, NULL);
	return (GStrv)g_ptr_array_free(out, FALSE);
}

/**
 * gowl_macro_run_by_name:
 * @compositor: a #GowlCompositor
 * @name: a macro name or path
 * @argv: (nullable) (array zero-terminated=1): its arguments
 *
 * Runs a macro through the macro module, as `macro-run' does -- the
 * entry point for C code that wants one.
 *
 * Returns: (transfer full) (nullable): the module's reply, %NULL when
 *   the macro module is not loaded
 */
gchar *
gowl_macro_run_by_name(
	GowlCompositor      *compositor,
	const gchar         *name,
	const gchar * const *argv
){
	g_autoptr(GString) line = NULL;
	g_autofree gchar *qname = NULL;
	guint i;

	g_return_val_if_fail(GOWL_IS_COMPOSITOR(compositor), NULL);
	g_return_val_if_fail(name != NULL, NULL);

	qname = g_shell_quote(name);
	line = g_string_new("macro-run ");
	g_string_append(line, qname);
	for (i = 0; argv != NULL && argv[i] != NULL; i++) {
		g_autofree gchar *q = g_shell_quote(argv[i]);

		g_string_append_c(line, ' ');
		g_string_append(line, q);
	}
	return gowl_compositor_run_command(compositor, line->str);
}
