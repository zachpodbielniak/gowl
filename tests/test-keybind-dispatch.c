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
 * deliberately not exercised.  The reload action is: with no monitors it
 * touches nothing but the config, and who releases which config is the
 * whole of what it can get wrong.
 */

#include <glib-object.h>
#include <glib/gstdio.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#include "core/gowl-core-private.h"
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

/*
 * The reload bind makes a fresh config from the file and hands it to the
 * compositor -- and it used to release the config the compositor had
 * been given first.  That one belongs to whoever gave it (main(), or an
 * embedder such as cmacs), which releases it after the compositor: the
 * reload freed it under its owner, set_config() disconnected a handler
 * from the freed memory at once, and the owner released it again later.
 * The compositor now releases only the configs it made itself -- the one
 * a later reload replaces, and its last one when it is finalized.
 *
 * Run in a subprocess with criticals fatal, from a directory of its own
 * whose data/config.yaml is the first file the search path finds.  The
 * file binds the key again, or the second reload would find no bind in
 * the config the first one made.
 */
static void
reload_keeps_the_given_config(void)
{
	static const gchar yaml_text[] =
		"border-width: 7\n"
		"keybinds:\n"
		"  \"Super+Shift+r\": { action: reload_config, desc: \"Reload\" }\n";
	g_autofree gchar *dir = NULL;
	g_autofree gchar *data = NULL;
	g_autofree gchar *yaml = NULL;
	GowlCompositor   *c;
	GowlConfig       *given;
	gpointer          first;
	gpointer          second;
	guint             mods;

	g_log_set_always_fatal(G_LOG_LEVEL_CRITICAL | G_LOG_FATAL_MASK);

	dir = g_dir_make_tmp("gowl-reload-XXXXXX", NULL);
	g_assert_nonnull(dir);
	data = g_build_filename(dir, "data", NULL);
	yaml = g_build_filename(data, "config.yaml", NULL);
	g_assert_cmpint(g_mkdir(data, 0700), ==, 0);
	g_assert_true(g_file_set_contents(yaml, yaml_text, -1, NULL));
	g_assert_cmpint(g_chdir(dir), ==, 0);
	/* Nothing of the user's may be found instead. */
	g_setenv("XDG_CONFIG_HOME", dir, TRUE);

	mods = GOWL_KEY_MOD_LOGO | GOWL_KEY_MOD_SHIFT;
	c = compositor_with_bind(mods, XKB_KEY_r, GOWL_ACTION_RELOAD_CONFIG,
	                         NULL, "Reload", &given);
	g_object_set(given, "border-width", 3, NULL);

	/* The first reload: a config made from the file, and the given one
	 * untouched. */
	g_assert_true(gowl_compositor_dispatch_keybind(c, mods, XKB_KEY_r));
	first = gowl_compositor_get_config(c);
	g_assert_true(first != (gpointer)given);
	g_assert_cmpint(gowl_config_get_border_width((GowlConfig *)first),
	                ==, 7);
	g_assert_true(GOWL_IS_CONFIG(given));
	g_assert_cmpint(gowl_config_get_border_width(given), ==, 3);
	g_object_add_weak_pointer(G_OBJECT(first), &first);

	/* The second releases the one the first made... */
	g_assert_true(gowl_compositor_dispatch_keybind(c, mods, XKB_KEY_r));
	second = gowl_compositor_get_config(c);
	g_assert_null(first);
	g_object_add_weak_pointer(G_OBJECT(second), &second);

	/* ...finalizing the compositor releases the last one... */
	g_object_unref(c);
	g_assert_null(second);

	/* ...and the given one is still its owner's to release. */
	g_assert_true(GOWL_IS_CONFIG(given));
	g_object_unref(given);

	g_assert_cmpint(g_chdir("/"), ==, 0);
	g_assert_cmpint(g_unlink(yaml), ==, 0);
	g_assert_cmpint(g_rmdir(data), ==, 0);
	g_assert_cmpint(g_rmdir(dir), ==, 0);
}

static void
test_reload_keeps_the_given_config(void)
{
	if (g_test_subprocess()) {
		reload_keeps_the_given_config();
		return;
	}
	g_test_trap_subprocess(NULL, 60 * G_USEC_PER_SEC,
	                       G_TEST_SUBPROCESS_INHERIT_STDERR);
	g_test_trap_assert_passed();
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

static void
test_move_stack(void)
{
	GowlConfig *cfg;
	GowlCompositor *comp = compositor_with_bind(GOWL_KEY_MOD_LOGO | GOWL_KEY_MOD_SHIFT,
		XKB_KEY_j, GOWL_ACTION_MOVE_STACK, "+1", "Move next", &cfg);
	GowlMonitor *mon = g_object_new(GOWL_TYPE_MONITOR, NULL);
	GowlClient *clients[6];
	gint i;

	mon->tagset[mon->seltags] = 1;
	comp->selmon = mon;
	for (i = 0; i < 6; i++) {
		clients[i] = gowl_client_new();
		clients[i]->mon = mon;
		clients[i]->tags = 1;
		comp->clients = g_list_append(comp->clients, clients[i]);
	}
	comp->fstack = g_list_prepend(NULL, clients[0]);
	clients[1]->isfloating = TRUE;
	clients[2]->tags = 2;
	clients[3]->isembedded = TRUE;
	clients[4]->isfullscreen = TRUE;
	g_assert_true(gowl_compositor_dispatch_keybind(comp,
		GOWL_KEY_MOD_LOGO | GOWL_KEY_MOD_SHIFT, XKB_KEY_j));
	g_assert_true(comp->clients->data == clients[5]);
	g_assert_true(g_list_last(comp->clients)->data == clients[0]);
	g_assert_true(comp->fstack->data == clients[0]);
	/* Forward wrap and reverse wrap skip the same ineligible clients. */
	gowl_compositor_move_stack(comp, 1);
	g_assert_true(comp->clients->data == clients[0]);
	gowl_compositor_move_stack(comp, -1);
	g_assert_true(g_list_last(comp->clients)->data == clients[0]);
	clients[0]->isfloating = TRUE;
	gowl_compositor_move_stack(comp, -1);
	g_assert_true(g_list_last(comp->clients)->data == clients[0]);
	clients[0]->isfloating = FALSE;
	clients[5]->tags = 2;
	gowl_compositor_move_stack(comp, -1);
	g_assert_true(g_list_last(comp->clients)->data == clients[0]);
	for (i = 0; i < 6; i++) {
		clients[i]->mon = NULL;
		g_object_unref(clients[i]);
	}
	comp->selmon = NULL;
	g_object_unref(comp);
	g_object_unref(mon);
	g_object_unref(cfg);
}

/* --- key modes and bind flags --- */

typedef struct {
	guint calls;
	gchar *last;
} ModeProbe;

static void
on_mode_changed(GowlCompositor *c, const gchar *mode, gpointer data)
{
	ModeProbe *p = (ModeProbe *)data;
	(void)c;

	p->calls++;
	g_free(p->last);
	p->last = g_strdup(mode);
}

static void
test_modes(void)
{
	GowlCompositor *c = gowl_compositor_new();
	GowlConfig     *cfg = gowl_config_new();
	CustomProbe     probe = { 0, NULL, FALSE };
	ModeProbe       mp = { 0, NULL };

	/* Super+r enters "resize"; inside it h runs a custom action and
	 * Escape leaves; h alone means nothing in the default mode. */
	gowl_config_add_keybind_full(cfg, GOWL_KEY_MOD_LOGO, XKB_KEY_r,
	                             GOWL_ACTION_MODE, "resize", NULL);
	gowl_config_add_keybind_ex(cfg, 0, XKB_KEY_h, GOWL_ACTION_CUSTOM,
	                           "(shrink)", NULL, "resize", 0);
	gowl_config_add_keybind_ex(cfg, 0, XKB_KEY_Escape, GOWL_ACTION_MODE,
	                           "default", NULL, "resize", 0);
	gowl_compositor_set_config(c, cfg);
	gowl_compositor_set_custom_action_handler(c, probe_custom, &probe);
	g_signal_connect(c, "mode-changed", G_CALLBACK(on_mode_changed), &mp);

	g_assert_cmpstr(gowl_compositor_get_key_mode(c), ==, "default");
	g_assert_false(gowl_compositor_dispatch_keybind(c, 0, XKB_KEY_h));
	g_assert_cmpuint(probe.calls, ==, 0);

	g_assert_true(gowl_compositor_dispatch_keybind(c, GOWL_KEY_MOD_LOGO, XKB_KEY_r));
	g_assert_cmpstr(gowl_compositor_get_key_mode(c), ==, "resize");
	g_assert_cmpuint(mp.calls, ==, 1);
	g_assert_cmpstr(mp.last, ==, "resize");

	/* In the mode: h fires, Super+r (a default-mode bind) does not. */
	g_assert_true(gowl_compositor_dispatch_keybind(c, 0, XKB_KEY_h));
	g_assert_cmpuint(probe.calls, ==, 1);
	g_assert_false(gowl_compositor_dispatch_keybind(c, GOWL_KEY_MOD_LOGO, XKB_KEY_r));

	g_assert_true(gowl_compositor_dispatch_keybind(c, 0, XKB_KEY_Escape));
	g_assert_cmpstr(gowl_compositor_get_key_mode(c), ==, "default");
	g_assert_cmpuint(mp.calls, ==, 2);
	g_assert_cmpstr(mp.last, ==, "default");
	/* Entering the mode one is in is not a change. */
	gowl_compositor_set_key_mode(c, NULL);
	gowl_compositor_set_key_mode(c, "");
	g_assert_cmpuint(mp.calls, ==, 2);

	g_free(mp.last);
	g_free(probe.last_arg);
	g_object_unref(c);
	g_object_unref(cfg);
}

static void
test_locked_and_release_flags(void)
{
	GowlCompositor *c = gowl_compositor_new();
	GowlConfig     *cfg = gowl_config_new();
	CustomProbe     probe = { 0, NULL, FALSE };

	gowl_config_add_keybind_ex(cfg, 0, XKB_KEY_XF86AudioMute,
	                           GOWL_ACTION_CUSTOM, "(mute)", NULL, NULL,
	                           GOWL_KEYBIND_FLAG_LOCKED);
	gowl_config_add_keybind_ex(cfg, GOWL_KEY_MOD_LOGO, XKB_KEY_x,
	                           GOWL_ACTION_CUSTOM, "(released)", NULL, NULL,
	                           GOWL_KEYBIND_FLAG_RELEASE);
	gowl_config_add_keybind_ex(cfg, GOWL_KEY_MOD_LOGO, XKB_KEY_n,
	                           GOWL_ACTION_CUSTOM, "(once)", NULL, NULL,
	                           GOWL_KEYBIND_FLAG_NO_REPEAT);
	gowl_compositor_set_config(c, cfg);
	gowl_compositor_set_custom_action_handler(c, probe_custom, &probe);

	/* Locked: only the locked bind answers. */
	g_assert_true(gowl_compositor_dispatch_key(c, 0, XKB_KEY_XF86AudioMute,
	                                           TRUE, TRUE));
	g_assert_cmpstr(probe.last_arg, ==, "(mute)");
	g_assert_false(gowl_compositor_dispatch_key(c, GOWL_KEY_MOD_LOGO, XKB_KEY_n,
	                                            TRUE, TRUE));
	/* Unlocked it answers as well. */
	g_assert_true(gowl_compositor_dispatch_key(c, 0, XKB_KEY_XF86AudioMute,
	                                           TRUE, FALSE));

	/* Release: not on the press, on the release. */
	g_assert_false(gowl_compositor_dispatch_key(c, GOWL_KEY_MOD_LOGO, XKB_KEY_x,
	                                            TRUE, FALSE));
	g_assert_true(gowl_compositor_dispatch_key(c, GOWL_KEY_MOD_LOGO, XKB_KEY_x,
	                                           FALSE, FALSE));
	g_assert_cmpstr(probe.last_arg, ==, "(released)");
	/* An ordinary bind does nothing on release. */
	g_assert_false(gowl_compositor_dispatch_key(c, GOWL_KEY_MOD_LOGO, XKB_KEY_n,
	                                            FALSE, FALSE));

	/* repeat: false is remembered for the key-repeat timer. */
	g_assert_true(gowl_compositor_dispatch_keybind(c, GOWL_KEY_MOD_LOGO, XKB_KEY_n));
	g_assert_false(c->kb_repeat_ok);
	g_assert_true(gowl_compositor_dispatch_key(c, 0, XKB_KEY_XF86AudioMute,
	                                           TRUE, FALSE));
	g_assert_true(c->kb_repeat_ok);

	g_free(probe.last_arg);
	g_object_unref(c);
	g_object_unref(cfg);
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
	g_test_add_func("/keybind-dispatch/reload-keeps-the-given-config",
	                test_reload_keeps_the_given_config);

	g_test_add_func("/keybind-dispatch/move-stack", test_move_stack);
	g_test_add_func("/keybind-dispatch/modes", test_modes);
	g_test_add_func("/keybind-dispatch/locked-and-release-flags",
	                test_locked_and_release_flags);

	return g_test_run();
}
