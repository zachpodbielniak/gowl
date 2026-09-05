/* test-keybind-dispatch.c -- keybind dispatch and the custom action
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * gowl_compositor_dispatch_keybind() is the path a real key press takes
 * into the config keybind table, so these run it directly rather than
 * synthesising input -- which would not work anyway: the seat's
 * send_key hands a key to the focused client and never consults the
 * table.
 *
 * Everything here uses a GowlCompositor that was never started.  It has
 * no backend, no display and no scene, which is exactly enough for the
 * lookup, the modifier matching, and the custom-action handoff.
 * Actions that touch the compositor's own state (tag-view, quit) are
 * deliberately not exercised.
 */

#include <glib-object.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#include "core/gowl-compositor.h"
#include "config/gowl-config.h"
#include "gowl-enums.h"

/* --- custom action handler --- */

typedef struct {
	guint    calls;
	gchar   *last_arg;
	gboolean saw_null;
} CustomProbe;

static gboolean
probe_custom(GowlCompositor *comp, const gchar *arg, gpointer data)
{
	CustomProbe *p = (CustomProbe *)data;

	(void)comp;
	p->calls++;
	if (arg == NULL)
		p->saw_null = TRUE;
	g_free(p->last_arg);
	p->last_arg = g_strdup(arg);
	return TRUE;
}

/* gowl_compositor_set_config() stores a BORROWED pointer -- it takes no
 * reference -- so the config has to outlive the compositor here.  Hand
 * it back through @out_cfg for the caller to unref last. */
static GowlCompositor *
compositor_with_bind(guint mods, guint sym, gint action,
                      const gchar *arg, const gchar *desc,
                      GowlConfig **out_cfg)
{
	GowlCompositor *c = gowl_compositor_new();
	GowlConfig     *cfg = gowl_config_new();

	gowl_config_add_keybind_full(cfg, mods, sym, action, arg, desc);
	gowl_compositor_set_config(c, cfg);

	*out_cfg = cfg;
	return c;
}

/*
 * The whole point of the custom action: the bind's arg reaches the
 * embedder verbatim.  The compositor attaches no meaning to it, so
 * anything that mangled it -- shell quoting, escaping, truncation --
 * would break every embedder differently.
 */
static void
test_custom_action_reaches_handler(void)
{
	GowlCompositor *c;
	GowlConfig     *cfg;
	CustomProbe     probe = { 0, NULL, FALSE };

	c = compositor_with_bind(GOWL_KEY_MOD_LOGO, XKB_KEY_F9,
	                          GOWL_ACTION_CUSTOM,
	                          "(cmacs-volume-raise 5)", "Volume up", &cfg);
	gowl_compositor_set_custom_action_handler(c, probe_custom, &probe);

	g_assert_true(gowl_compositor_dispatch_keybind(
	                      c, GOWL_KEY_MOD_LOGO, XKB_KEY_F9));
	g_assert_cmpuint(probe.calls, ==, 1);
	g_assert_cmpstr(probe.last_arg, ==, "(cmacs-volume-raise 5)");

	g_free(probe.last_arg);
	g_object_unref(c);
	g_object_unref(cfg);
}

/*
 * With no handler installed a custom bind is a no-op that still
 * consumes the key.  This is the behaviour every gowl before the hook
 * existed had, and standalone gowl still has it -- an inherited config
 * with custom binds must not start erroring.
 */
static void
test_custom_action_without_handler_is_consumed(void)
{
	GowlCompositor *c;
	GowlConfig     *cfg;

	c = compositor_with_bind(GOWL_KEY_MOD_LOGO, XKB_KEY_F9,
	                          GOWL_ACTION_CUSTOM, "(anything)", NULL, &cfg);

	g_assert_true(gowl_compositor_dispatch_keybind(
	                      c, GOWL_KEY_MOD_LOGO, XKB_KEY_F9));

	g_object_unref(c);
	g_object_unref(cfg);
}

/* Clearing the handler puts it back to the no-op. */
static void
test_custom_action_handler_can_be_cleared(void)
{
	GowlCompositor *c;
	GowlConfig     *cfg;
	CustomProbe     probe = { 0, NULL, FALSE };

	c = compositor_with_bind(GOWL_KEY_MOD_LOGO, XKB_KEY_F9,
	                          GOWL_ACTION_CUSTOM, "(x)", NULL, &cfg);

	gowl_compositor_set_custom_action_handler(c, probe_custom, &probe);
	gowl_compositor_dispatch_keybind(c, GOWL_KEY_MOD_LOGO, XKB_KEY_F9);
	g_assert_cmpuint(probe.calls, ==, 1);

	gowl_compositor_set_custom_action_handler(c, NULL, NULL);
	gowl_compositor_dispatch_keybind(c, GOWL_KEY_MOD_LOGO, XKB_KEY_F9);
	g_assert_cmpuint(probe.calls, ==, 1);

	g_free(probe.last_arg);
	g_object_unref(c);
	g_object_unref(cfg);
}

/* An unbound combination reports no match rather than swallowing it. */
static void
test_unbound_key_does_not_match(void)
{
	GowlCompositor *c;
	GowlConfig     *cfg;

	c = compositor_with_bind(GOWL_KEY_MOD_LOGO, XKB_KEY_F9,
	                          GOWL_ACTION_CUSTOM, "(x)", NULL, &cfg);

	g_assert_false(gowl_compositor_dispatch_keybind(
	                       c, GOWL_KEY_MOD_LOGO, XKB_KEY_F10));
	g_assert_false(gowl_compositor_dispatch_keybind(
	                       c, 0, XKB_KEY_F9));

	g_object_unref(c);
	g_object_unref(cfg);
}

/*
 * A media key carries no modifier.  Dispatch compares cleaned masks, so
 * zero matches zero -- this is what makes the shipped XF86 binds work
 * without a Super in front of them.
 */
static void
test_modifierless_media_key_dispatches(void)
{
	GowlCompositor *c;
	GowlConfig     *cfg;
	CustomProbe     probe = { 0, NULL, FALSE };

	c = compositor_with_bind(0, XKB_KEY_XF86AudioRaiseVolume,
	                          GOWL_ACTION_CUSTOM,
	                          "(cmacs-gowl-volume-raise)", "Volume up", &cfg);
	gowl_compositor_set_custom_action_handler(c, probe_custom, &probe);

	g_assert_true(gowl_compositor_dispatch_keybind(
	                      c, 0, XKB_KEY_XF86AudioRaiseVolume));
	g_assert_cmpuint(probe.calls, ==, 1);

	g_free(probe.last_arg);
	g_object_unref(c);
	g_object_unref(cfg);
}

/*
 * NumLock and CapsLock must not stop a bind from firing.  Dispatch
 * cleans the incoming mask, which is why a laptop with NumLock on does
 * not lose every keybind.
 */
static void
test_lock_modifiers_are_ignored(void)
{
	GowlCompositor *c;
	GowlConfig     *cfg;
	CustomProbe     probe = { 0, NULL, FALSE };

	c = compositor_with_bind(GOWL_KEY_MOD_LOGO, XKB_KEY_F9,
	                          GOWL_ACTION_CUSTOM, "(x)", NULL, &cfg);
	gowl_compositor_set_custom_action_handler(c, probe_custom, &probe);

	g_assert_true(gowl_compositor_dispatch_keybind(
	                      c,
	                      GOWL_KEY_MOD_LOGO | GOWL_KEY_MOD_CAPS
	                      | GOWL_KEY_MOD_MOD2,
	                      XKB_KEY_F9));
	g_assert_cmpuint(probe.calls, ==, 1);

	g_free(probe.last_arg);
	g_object_unref(c);
	g_object_unref(cfg);
}

/* A custom bind with no arg still calls the handler, with NULL. */
static void
test_custom_action_null_arg(void)
{
	GowlCompositor *c;
	GowlConfig     *cfg;
	CustomProbe     probe = { 0, NULL, FALSE };

	c = compositor_with_bind(GOWL_KEY_MOD_LOGO, XKB_KEY_F9,
	                          GOWL_ACTION_CUSTOM, NULL, NULL, &cfg);
	gowl_compositor_set_custom_action_handler(c, probe_custom, &probe);

	g_assert_true(gowl_compositor_dispatch_keybind(
	                      c, GOWL_KEY_MOD_LOGO, XKB_KEY_F9));
	g_assert_cmpuint(probe.calls, ==, 1);
	g_assert_true(probe.saw_null);

	g_free(probe.last_arg);
	g_object_unref(c);
	g_object_unref(cfg);
}

/* Dispatch with no config at all is FALSE, not a crash. */
static void
test_dispatch_without_config(void)
{
	GowlCompositor *c = gowl_compositor_new();

	g_assert_false(gowl_compositor_dispatch_keybind(
	                       c, GOWL_KEY_MOD_LOGO, XKB_KEY_F9));

	g_object_unref(c);
}

int
main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/keybind-dispatch/custom-reaches-handler",
	                test_custom_action_reaches_handler);
	g_test_add_func("/keybind-dispatch/custom-without-handler",
	                test_custom_action_without_handler_is_consumed);
	g_test_add_func("/keybind-dispatch/custom-handler-cleared",
	                test_custom_action_handler_can_be_cleared);
	g_test_add_func("/keybind-dispatch/unbound-key",
	                test_unbound_key_does_not_match);
	g_test_add_func("/keybind-dispatch/modifierless-media-key",
	                test_modifierless_media_key_dispatches);
	g_test_add_func("/keybind-dispatch/lock-modifiers-ignored",
	                test_lock_modifiers_are_ignored);
	g_test_add_func("/keybind-dispatch/custom-null-arg",
	                test_custom_action_null_arg);
	g_test_add_func("/keybind-dispatch/no-config",
	                test_dispatch_without_config);

	return g_test_run();
}
