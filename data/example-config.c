/*
 * gowl - Example C Configuration
 * GObject Wayland Compositor
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
 *
 * Install: cp example-config.c ~/.config/gowl/config.c
 * Compile: gowl --recompile
 *
 * This file is compiled to a .so and loaded at startup.
 * On compile failure, YAML config / defaults are used.
 *
 * Optional build args override:
 *   #define GOWL_BUILD_ARGS "-I/custom/path"
 */

#include <gowl/gowl.h>
#include <xkbcommon/xkbcommon-keysyms.h>

/*
 * Extern references to compositor objects.
 * These are resolved at dlopen time from the running compositor.
 */
extern GowlCompositor *gowl_compositor;
extern GowlConfig     *gowl_config;

/* Shorthand macros for modifier combinations */
#define SUPER       GOWL_KEY_MOD_LOGO
#define SUPER_SHIFT (GOWL_KEY_MOD_LOGO | GOWL_KEY_MOD_SHIFT)
#define SUPER_CTRL  (GOWL_KEY_MOD_LOGO | GOWL_KEY_MOD_CTRL)
#define SUPER_SHIFT_CTRL \
	(GOWL_KEY_MOD_LOGO | GOWL_KEY_MOD_SHIFT | GOWL_KEY_MOD_CTRL)

/* Shorthand for adding a keybind */
#define KEYBIND(mods, key, act, argument) \
	gowl_config_add_keybind(gowl_config, (mods), (key), (act), (argument))

/* Shorthand for adding a window rule */
#define RULE(id, title, tags, floating, mon) \
	gowl_config_add_rule(gowl_config, (id), (title), (tags), (floating), (mon))

/**
 * add_keybinds:
 *
 * Registers all keybinds with the config.
 */
static void
add_keybinds(void)
{
	gint i;

	/* Launch applications */
	KEYBIND(SUPER, XKB_KEY_Return, GOWL_ACTION_SPAWN, "gst");
	KEYBIND(SUPER, XKB_KEY_p,     GOWL_ACTION_SPAWN, "bemenu-run");

	/* Client management */
	KEYBIND(SUPER_SHIFT, XKB_KEY_c,      GOWL_ACTION_KILL_CLIENT, NULL);
	KEYBIND(SUPER,       XKB_KEY_space,   GOWL_ACTION_TOGGLE_FLOAT, NULL);
	KEYBIND(SUPER_SHIFT, XKB_KEY_space,   GOWL_ACTION_TOGGLE_FULLSCREEN, NULL);
	KEYBIND(SUPER_SHIFT, XKB_KEY_Return,  GOWL_ACTION_ZOOM, NULL);

	/* Focus navigation */
	KEYBIND(SUPER, XKB_KEY_j, GOWL_ACTION_FOCUS_STACK, "+1");
	KEYBIND(SUPER, XKB_KEY_k, GOWL_ACTION_FOCUS_STACK, "-1");

	/* Master area adjustment */
	KEYBIND(SUPER, XKB_KEY_h, GOWL_ACTION_SET_MFACT,    "-0.05");
	KEYBIND(SUPER, XKB_KEY_l, GOWL_ACTION_SET_MFACT,    "+0.05");
	KEYBIND(SUPER, XKB_KEY_i, GOWL_ACTION_INC_NMASTER,  "+1");
	KEYBIND(SUPER, XKB_KEY_d, GOWL_ACTION_INC_NMASTER,  "-1");

	/* Layout selection */
	KEYBIND(SUPER, XKB_KEY_t, GOWL_ACTION_SET_LAYOUT, "tile");
	KEYBIND(SUPER, XKB_KEY_f, GOWL_ACTION_SET_LAYOUT, "float");
	KEYBIND(SUPER, XKB_KEY_m, GOWL_ACTION_SET_LAYOUT, "monocle");

	/* Multi-monitor */
	KEYBIND(SUPER,       XKB_KEY_comma,  GOWL_ACTION_FOCUS_MONITOR,   "-1");
	KEYBIND(SUPER,       XKB_KEY_period, GOWL_ACTION_FOCUS_MONITOR,   "+1");
	KEYBIND(SUPER_SHIFT, XKB_KEY_comma,  GOWL_ACTION_MOVE_TO_MONITOR, "-1");
	KEYBIND(SUPER_SHIFT, XKB_KEY_period, GOWL_ACTION_MOVE_TO_MONITOR, "+1");

	/* View all tags / tag all */
	KEYBIND(SUPER,       XKB_KEY_0, GOWL_ACTION_TAG_VIEW, "0");
	KEYBIND(SUPER_SHIFT, XKB_KEY_0, GOWL_ACTION_TAG_SET,  "0");

	/* Tag switching, assignment, toggle view, and toggle (1-9) */
	for (i = 1; i <= 9; i++) {
		gchar arg[4];

		g_snprintf(arg, sizeof(arg), "%d", i);

		KEYBIND(SUPER,            XKB_KEY_1 + i - 1,
		        GOWL_ACTION_TAG_VIEW, arg);
		KEYBIND(SUPER_SHIFT,      XKB_KEY_1 + i - 1,
		        GOWL_ACTION_TAG_SET, arg);
		KEYBIND(SUPER_CTRL,       XKB_KEY_1 + i - 1,
		        GOWL_ACTION_TAG_TOGGLE_VIEW, arg);
		KEYBIND(SUPER_SHIFT_CTRL, XKB_KEY_1 + i - 1,
		        GOWL_ACTION_TAG_TOGGLE, arg);
	}

	/* Session */
	KEYBIND(SUPER_SHIFT, XKB_KEY_q, GOWL_ACTION_QUIT, NULL);
	KEYBIND(SUPER_SHIFT, XKB_KEY_r, GOWL_ACTION_RELOAD_CONFIG, NULL);
}

/**
 * add_rules:
 *
 * Registers window rules with the config.
 */
static void
add_rules(void)
{
	RULE("firefox",                  NULL, 2, FALSE, -1);
	RULE("thunderbird",              NULL, 4, FALSE, -1);
	RULE("org.keepassxc.KeePassXC",  NULL, 8, TRUE,  -1);
	RULE("pavucontrol",              NULL, 0, TRUE,  -1);
	RULE("imv",                      NULL, 0, TRUE,  -1);
}

/**
 * gowl_config_init:
 *
 * Called after YAML config is loaded but before compositor starts.
 * Override or supplement YAML values here.
 * Return TRUE on success, FALSE to fall back to defaults.
 */
G_MODULE_EXPORT gboolean
gowl_config_init(void)
{
	/* Compositor settings */
	g_object_set(gowl_config,
		"log-level",    "warning",
		"repeat-rate",  25,
		"repeat-delay", 600,
		"terminal",     "gst",
		"menu",         "bemenu-run",
		"sloppyfocus",  TRUE,
		NULL);

	/* Appearance */
	g_object_set(gowl_config,
		"border-width",         2,
		"border-color-focus",   "#005577",
		"border-color-unfocus", "#444444",
		"border-color-urgent",  "#ff0000",
		NULL);

	/* Layout */
	g_object_set(gowl_config,
		"mfact",   0.55,
		"nmaster", 1,
		NULL);

	/* Tags */
	g_object_set(gowl_config,
		"tag-count", 9,
		NULL);

	/* Keybinds */
	add_keybinds();

	/* Window rules */
	add_rules();

	return TRUE;
}
