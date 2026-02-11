/*
 * gowl - Default C Configuration
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
 * Compile with:
 *   gcc $(pkg-config --cflags --libs glib-2.0 gobject-2.0 gmodule-2.0) \
 *       -std=gnu89 -shared -fPIC -o config.so config.c
 *
 * Or let gowl auto-compile this file from:
 *   ~/.config/gowl/config.c
 *
 * The GOWL_BUILD_ARGS define below is optional. If present, the
 * config compiler extracts it and passes the value as extra flags
 * to gcc. Remove or modify it if you need custom include paths
 * or additional libraries.
 */

#define GOWL_BUILD_ARGS "-std=gnu89 -shared -fPIC"

#include <gowl/gowl.h>
#include <xkbcommon/xkbcommon.h>

/*
 * Extern references to compositor-owned singletons.
 * These symbols are resolved at dlopen time from the running
 * compositor process. They are valid for the entire lifetime
 * of the compositor.
 */
extern GowlCompositor *gowl_compositor;
extern GowlConfig     *gowl_config;

/* --- Helper macros --- */

/* Shorthand for GOWL_KEY_MOD_LOGO (Super key) */
#define MODKEY GOWL_KEY_MOD_LOGO

/* Tag bitmask from 1-based tag number (0 = all tags) */
#define TAGMASK(n) ((n) == 0 ? GOWL_TAGMASK_ALL(9) : GOWL_TAGMASK((n) - 1))

/**
 * gowl_config_init:
 *
 * Entry point called by the config compiler after this shared object
 * is loaded.  This runs after the YAML config has been applied, so
 * any values set here override YAML.
 *
 * Use g_object_set() on gowl_config to change properties, and
 * gowl_config_add_keybind() / gowl_config_add_rule() to add
 * keybinds and window rules.
 *
 * Returns: TRUE on success, FALSE to fall back to defaults.
 */
G_MODULE_EXPORT gboolean
gowl_config_init(void)
{
	/* --- Appearance --- */
	g_object_set(gowl_config,
		"border-width",         2,
		"border-color-focus",   "#005577",
		"border-color-unfocus", "#444444",
		"border-color-urgent",  "#ff0000",
		NULL);

	/* --- Layout --- */
	g_object_set(gowl_config,
		"mfact",   0.55,
		"nmaster", 1,
		NULL);

	/* --- Input --- */
	g_object_set(gowl_config,
		"repeat-rate",  25,
		"repeat-delay", 600,
		NULL);

	/* --- General --- */
	g_object_set(gowl_config,
		"tag-count",   9,
		"terminal",    "foot",
		"menu",        "bemenu-run",
		"sloppyfocus", TRUE,
		"log-level",   "warning",
		NULL);

	/* --- Keybinds --- */

	/* Launch terminal */
	gowl_config_add_keybind(gowl_config,
		MODKEY, XKB_KEY_Return,
		GOWL_ACTION_SPAWN, "foot");

	/* Launch menu */
	gowl_config_add_keybind(gowl_config,
		MODKEY, XKB_KEY_p,
		GOWL_ACTION_SPAWN, "bemenu-run");

	/* Kill focused client */
	gowl_config_add_keybind(gowl_config,
		MODKEY | GOWL_KEY_MOD_SHIFT, XKB_KEY_c,
		GOWL_ACTION_KILL_CLIENT, NULL);

	/* Focus next/prev in stack */
	gowl_config_add_keybind(gowl_config,
		MODKEY, XKB_KEY_j,
		GOWL_ACTION_FOCUS_STACK, "+1");

	gowl_config_add_keybind(gowl_config,
		MODKEY, XKB_KEY_k,
		GOWL_ACTION_FOCUS_STACK, "-1");

	/* Adjust master factor */
	gowl_config_add_keybind(gowl_config,
		MODKEY, XKB_KEY_h,
		GOWL_ACTION_SET_MFACT, "-0.05");

	gowl_config_add_keybind(gowl_config,
		MODKEY, XKB_KEY_l,
		GOWL_ACTION_SET_MFACT, "+0.05");

	/* Adjust nmaster */
	gowl_config_add_keybind(gowl_config,
		MODKEY, XKB_KEY_i,
		GOWL_ACTION_INC_NMASTER, "+1");

	gowl_config_add_keybind(gowl_config,
		MODKEY, XKB_KEY_d,
		GOWL_ACTION_INC_NMASTER, "-1");

	/* Zoom (promote to master) */
	gowl_config_add_keybind(gowl_config,
		MODKEY | GOWL_KEY_MOD_SHIFT, XKB_KEY_Return,
		GOWL_ACTION_ZOOM, NULL);

	/* Layout selection */
	gowl_config_add_keybind(gowl_config,
		MODKEY, XKB_KEY_t,
		GOWL_ACTION_SET_LAYOUT, "tile");

	gowl_config_add_keybind(gowl_config,
		MODKEY, XKB_KEY_f,
		GOWL_ACTION_SET_LAYOUT, "float");

	gowl_config_add_keybind(gowl_config,
		MODKEY, XKB_KEY_m,
		GOWL_ACTION_SET_LAYOUT, "monocle");

	/* Toggle floating / fullscreen */
	gowl_config_add_keybind(gowl_config,
		MODKEY, XKB_KEY_space,
		GOWL_ACTION_TOGGLE_FLOAT, NULL);

	gowl_config_add_keybind(gowl_config,
		MODKEY | GOWL_KEY_MOD_SHIFT, XKB_KEY_space,
		GOWL_ACTION_TOGGLE_FULLSCREEN, NULL);

	/* View all tags */
	gowl_config_add_keybind(gowl_config,
		MODKEY, XKB_KEY_0,
		GOWL_ACTION_TAG_VIEW, "0");

	/* Tag focused client to all tags */
	gowl_config_add_keybind(gowl_config,
		MODKEY | GOWL_KEY_MOD_SHIFT, XKB_KEY_0,
		GOWL_ACTION_TAG_SET, "0");

	/* Per-tag keybinds: Super+1..9 to view, Super+Shift+1..9 to tag */
	{
		guint n;
		gchar arg_buf[4];

		for (n = 1; n <= 9; n++) {
			g_snprintf(arg_buf, sizeof(arg_buf), "%u", n);

			gowl_config_add_keybind(gowl_config,
				MODKEY, XKB_KEY_1 + (n - 1),
				GOWL_ACTION_TAG_VIEW, arg_buf);

			gowl_config_add_keybind(gowl_config,
				MODKEY | GOWL_KEY_MOD_SHIFT,
				XKB_KEY_1 + (n - 1),
				GOWL_ACTION_TAG_SET, arg_buf);

			gowl_config_add_keybind(gowl_config,
				MODKEY | GOWL_KEY_MOD_CTRL,
				XKB_KEY_1 + (n - 1),
				GOWL_ACTION_TAG_TOGGLE_VIEW, arg_buf);

			gowl_config_add_keybind(gowl_config,
				MODKEY | GOWL_KEY_MOD_SHIFT | GOWL_KEY_MOD_CTRL,
				XKB_KEY_1 + (n - 1),
				GOWL_ACTION_TAG_TOGGLE, arg_buf);
		}
	}

	/* Multi-monitor focus */
	gowl_config_add_keybind(gowl_config,
		MODKEY, XKB_KEY_comma,
		GOWL_ACTION_FOCUS_MONITOR, "-1");

	gowl_config_add_keybind(gowl_config,
		MODKEY, XKB_KEY_period,
		GOWL_ACTION_FOCUS_MONITOR, "+1");

	/* Multi-monitor move client */
	gowl_config_add_keybind(gowl_config,
		MODKEY | GOWL_KEY_MOD_SHIFT, XKB_KEY_comma,
		GOWL_ACTION_MOVE_TO_MONITOR, "-1");

	gowl_config_add_keybind(gowl_config,
		MODKEY | GOWL_KEY_MOD_SHIFT, XKB_KEY_period,
		GOWL_ACTION_MOVE_TO_MONITOR, "+1");

	/* Quit / reload */
	gowl_config_add_keybind(gowl_config,
		MODKEY | GOWL_KEY_MOD_SHIFT, XKB_KEY_q,
		GOWL_ACTION_QUIT, NULL);

	gowl_config_add_keybind(gowl_config,
		MODKEY | GOWL_KEY_MOD_SHIFT, XKB_KEY_r,
		GOWL_ACTION_RELOAD_CONFIG, NULL);

	/* --- Window Rules --- */

	/* Firefox on tag 2 */
	gowl_config_add_rule(gowl_config,
		"firefox", NULL,
		TAGMASK(2), FALSE, -1);

	/* Thunderbird on tag 3 */
	gowl_config_add_rule(gowl_config,
		"thunderbird", NULL,
		TAGMASK(4), FALSE, -1);

	/* KeePassXC floating on tag 4 */
	gowl_config_add_rule(gowl_config,
		"org.keepassxc.KeePassXC", NULL,
		TAGMASK(8), TRUE, -1);

	/* pavucontrol always floating */
	gowl_config_add_rule(gowl_config,
		"pavucontrol", NULL,
		0, TRUE, -1);

	/* imv image viewer always floating */
	gowl_config_add_rule(gowl_config,
		"imv", NULL,
		0, TRUE, -1);

	return TRUE;
}
