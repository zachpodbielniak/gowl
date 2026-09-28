/*
 * hello.c - the smallest macro: say hello, report what started it.
 *
 * gowl - GObject Wayland Compositor.  Example macro.
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Run it:   gowl-msg macro-run hello world
 * Copy it to ~/.config/gowl/macros/ as the start of your own.
 */

#include <gowl/gowl.h>

/* Optional: a one-line description, shown by `macro-info' and lists. */
G_MODULE_EXPORT const gchar *
gowl_macro_info(void)
{
	return "Say hello and show what started the macro";
}

/* Required: the macro itself.  Return FALSE to report failure. */
G_MODULE_EXPORT gboolean
gowl_macro_run(GowlMacroContext *ctx)
{
	GEnumClass *klass;
	GEnumValue *trigger;
	const gchar *who;

	klass = (GEnumClass *)g_type_class_ref(GOWL_TYPE_MACRO_TRIGGER);
	trigger = g_enum_get_value(klass, (gint)gowl_macro_get_trigger(ctx));
	who = gowl_macro_get_arg(ctx, 0);

	gowl_macro_notify(ctx, "Hello from a macro",
	                  who != NULL ? who : "no argument given");
	gowl_macro_log(ctx, "started by %s (%s)",
	               trigger != NULL ? trigger->value_nick : "?",
	               gowl_macro_get_trigger_detail(ctx));
	g_type_class_unref(klass);

	/* What `macro-run' replies with, after "OK " */
	gowl_macro_set_result(ctx, "hello");
	return TRUE;
}
