/*
 * gowl - Example Bar C Configuration
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
 * Install: cp example-bar.c ~/.config/gowl/bar.c
 * Compile: gowlbar --recompile
 *
 * This file is compiled to a .so and loaded at startup.
 * On compile failure, YAML config / defaults are used.
 *
 * Optional build args override:
 *   #define GOWLBAR_BUILD_ARGS "-I/custom/path"
 */

#include <glib-object.h>
#include <gmodule.h>

/*
 * Extern references to bar objects.
 * These are resolved at dlopen time from the running gowlbar process.
 */
extern GObject *gowlbar_app;
extern GObject *gowlbar_config;

/**
 * gowlbar_config_init:
 *
 * Called after YAML config is loaded but before the bar starts.
 * Override or supplement YAML values here.
 * Return TRUE on success, FALSE to fall back to defaults.
 */
G_MODULE_EXPORT gboolean
gowlbar_config_init(void)
{
	/* Bar settings */
	g_object_set(gowlbar_config,
		"height",   24,
		"position", "top",
		"font",     "monospace 10",
		"padding",  4,
		NULL);

	/* Colours */
	g_object_set(gowlbar_config,
		"background",      "#1e1e2e",
		"foreground",      "#cdd6f4",
		"tag-active-bg",   "#89b4fa",
		"tag-active-fg",   "#1e1e2e",
		"tag-urgent-bg",   "#f38ba8",
		"tag-urgent-fg",   "#1e1e2e",
		"tag-occupied-fg", "#cdd6f4",
		"tag-empty-fg",    "#585b70",
		"layout-fg",       "#a6adc8",
		"title-fg",        "#cdd6f4",
		"status-fg",       "#a6adc8",
		"border-color",    "#313244",
		NULL);

	return TRUE;
}
