/*
 * pedal-demo.c - what a macro started by the input remapper sees.
 *
 * gowl - GObject Wayland Compositor.  Example macro.
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Bind it to a pedal (or any key of a claimed device):
 *     input-remap:
 *       - name: pedals
 *         match: { id: "1a86:e026" }
 *         map:
 *           KEY_C: { macro: pedal-demo, args: "hello" }
 *
 * The trigger is `remap' and the detail names the rule and the input
 * ("pedals KEY_C").  A remap macro runs on the PRESS only.
 *
 * A macro is not one-to-one: one press can do many things.  Do not
 * bind macros where a game's rules forbid automation; use the
 * remapper's plain targets there.
 */

#include <gowl/gowl.h>

G_MODULE_EXPORT const gchar *
gowl_macro_info(void)
{
	return "Show the trigger, detail and arguments a remap macro receives";
}

G_MODULE_EXPORT gboolean
gowl_macro_run(GowlMacroContext *ctx)
{
	g_autofree gchar *args = NULL;
	g_autofree gchar *body = NULL;

	args = g_strjoinv(" ", (gchar **)gowl_macro_get_argv(ctx));
	body = g_strdup_printf("%s: %s -- args: %s",
	                       gowl_macro_get_trigger(ctx) == GOWL_MACRO_TRIGGER_REMAP
	                       ? "remap" : "other trigger",
	                       gowl_macro_get_trigger_detail(ctx), args);
	gowl_macro_notify(ctx, "Pedal pressed", body);
	gowl_macro_set_result(ctx, body);
	return TRUE;
}
