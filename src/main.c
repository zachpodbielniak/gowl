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

#include "gowl.h"
#include "ipc/gowl-ipc.h"
#include <stdio.h>
#include <stdlib.h>

/*
 * Global pointers exported for C config shared objects.
 * The user's config.c declares these as extern and accesses
 * them from gowl_config_init() at dlopen time.
 */
GowlCompositor *gowl_compositor = NULL;
GowlConfig     *gowl_config = NULL;

/* Default YAML config content for --generate-yaml-config */
static const gchar *default_yaml_config =
	"# Gowl Default Configuration\n"
	"# Search order: ./data/ > ~/.config/gowl/ > /etc/gowl/ > /usr/local/gowl/\n"
	"\n"
	"compositor:\n"
	"  log_level: warning\n"
	"  log_file: \"~/.config/gowl/gowl.log\"\n"
	"  repeat_rate: 25\n"
	"  repeat_delay: 600\n"
	"  terminal: gst\n"
	"  menu: bemenu-run\n"
	"  sloppyfocus: true\n"
	"\n"
	"appearance:\n"
	"  border_width: 2\n"
	"  border_color_focus: \"#bbbbbb\"\n"
	"  border_color_unfocus: \"#444444\"\n"
	"  border_color_urgent: \"#ff0000\"\n"
	"\n"
	"layout:\n"
	"  default: tile\n"
	"  mfact: 0.55\n"
	"  nmaster: 1\n"
	"\n"
	"tags:\n"
	"  count: 9\n"
	"\n"
	"keybinds:\n"
	"  \"Super+Return\": { action: spawn, arg: \"gst\" }\n"
	"  \"Super+p\": { action: spawn, arg: \"bemenu-run\" }\n"
	"  \"Super+Shift+c\": { action: kill_client }\n"
	"  \"Super+j\": { action: focus_stack, arg: \"+1\" }\n"
	"  \"Super+k\": { action: focus_stack, arg: \"-1\" }\n"
	"  \"Super+i\": { action: inc_nmaster, arg: \"+1\" }\n"
	"  \"Super+d\": { action: inc_nmaster, arg: \"-1\" }\n"
	"  \"Super+h\": { action: set_mfact, arg: \"-0.05\" }\n"
	"  \"Super+l\": { action: set_mfact, arg: \"+0.05\" }\n"
	"  \"Super+t\": { action: set_layout, arg: \"tile\" }\n"
	"  \"Super+f\": { action: toggle_fullscreen }\n"
	"  \"Super+space\": { action: toggle_float }\n"
	"  \"Super+Shift+q\": { action: quit }\n"
	"\n"
	"rules: []\n"
	"\n"
	"autostart: []\n"
	"\n"
	"monitors: {}\n"
	"\n"
	"modules:\n"
	"  vanitygaps:\n"
	"    enabled: false\n"
	"  pertag:\n"
	"    enabled: false\n"
	"  autostart:\n"
	"    enabled: true\n";

/* Default C config template for --generate-c-config */
static const gchar *default_c_config =
	"/*\n"
	" * gowl user configuration\n"
	" *\n"
	" * Compile args are taken from the GOWL_BUILD_ARGS define if present,\n"
	" * otherwise pkg-config is used. This file is compiled to a .so and\n"
	" * loaded at startup. On compile failure, defaults are used.\n"
	" *\n"
	" * Optional build args override:\n"
	" * #define GOWL_BUILD_ARGS \"-I/custom/path\"\n"
	" */\n"
	"\n"
	"#include <gowl/gowl.h>\n"
	"\n"
	"/*\n"
	" * Extern references to compositor objects.\n"
	" * These are resolved at dlopen time from the running compositor.\n"
	" */\n"
	"extern GowlCompositor *gowl_compositor;\n"
	"extern GowlConfig     *gowl_config;\n"
	"\n"
	"/*\n"
	" * gowl_config_init:\n"
	" *\n"
	" * Called after YAML config is loaded but before compositor starts.\n"
	" * Override or supplement YAML values here.\n"
	" * Return TRUE on success, FALSE to fall back to defaults.\n"
	" */\n"
	"G_MODULE_EXPORT gboolean\n"
	"gowl_config_init(void)\n"
	"{\n"
	"    /* Example: override border width */\n"
	"    g_object_set(gowl_config,\n"
	"        \"border-width\", 3,\n"
	"        \"mfact\", 0.55,\n"
	"        NULL);\n"
	"\n"
	"    return TRUE;\n"
	"}\n";

int
main(int argc, char *argv[])
{
	gboolean show_version = FALSE;
	gboolean debug_mode = FALSE;
	gboolean generate_yaml = FALSE;
	gboolean generate_c = FALSE;
	gboolean no_c_config = FALSE;
	gboolean recompile = FALSE;
	gchar *config_path = NULL;
	gchar *c_config_path = NULL;
	gchar *startup_cmd = NULL;
	GError *error = NULL;
	GowlConfig *config = NULL;
	GowlCompositor *compositor = NULL;
	GowlModuleManager *module_mgr = NULL;
	GowlIpc *ipc = NULL;
	int ret = 0;

	GOptionEntry entries[] = {
		{ "version", 'v', 0, G_OPTION_ARG_NONE, &show_version,
			"Show version information", NULL },
		{ "debug", 'd', 0, G_OPTION_ARG_NONE, &debug_mode,
			"Enable debug logging", NULL },
		{ "generate-yaml-config", 0, 0, G_OPTION_ARG_NONE, &generate_yaml,
			"Print default YAML config to stdout", NULL },
		{ "generate-c-config", 0, 0, G_OPTION_ARG_NONE, &generate_c,
			"Print default C config template to stdout", NULL },
		{ "config", 0, 0, G_OPTION_ARG_FILENAME, &config_path,
			"Override YAML config path", "PATH" },
		{ "c-config", 0, 0, G_OPTION_ARG_FILENAME, &c_config_path,
			"Override C config path", "PATH" },
		{ "no-c-config", 0, 0, G_OPTION_ARG_NONE, &no_c_config,
			"Skip C config compilation", NULL },
		{ "recompile", 0, 0, G_OPTION_ARG_NONE, &recompile,
			"Compile C config and exit", NULL },
		{ "startup", 's', 0, G_OPTION_ARG_STRING, &startup_cmd,
			"Startup command", "CMD" },
		{ NULL }
	};

	GOptionContext *opt_ctx;

	opt_ctx = g_option_context_new("- GObject Wayland Compositor");
	g_option_context_add_main_entries(opt_ctx, entries, NULL);

	if (!g_option_context_parse(opt_ctx, &argc, &argv, &error)) {
		g_printerr("Error parsing options: %s\n", error->message);
		g_error_free(error);
		g_option_context_free(opt_ctx);
		return 1;
	}
	g_option_context_free(opt_ctx);

	/* Handle --version */
	if (show_version) {
		g_print("gowl %s\n", GOWL_VERSION_STRING);
		ret = 0;
		goto cleanup;
	}

	/* Handle --generate-yaml-config */
	if (generate_yaml) {
		g_print("%s", default_yaml_config);
		ret = 0;
		goto cleanup;
	}

	/* Handle --generate-c-config */
	if (generate_c) {
		g_print("%s", default_c_config);
		ret = 0;
		goto cleanup;
	}

	/* Handle --recompile */
	if (recompile) {
		g_autoptr(GowlConfigCompiler) compiler = NULL;
		g_autofree gchar *c_source = NULL;
		g_autofree gchar *so_path = NULL;

		compiler = gowl_config_compiler_new();
		so_path  = gowl_config_compiler_get_cache_path(compiler);

		if (c_config_path != NULL) {
			c_source = g_strdup(c_config_path);
		} else {
			const gchar *search_dirs[] = {
				"./data",
				NULL,
				GOWL_SYSCONFDIR "/gowl",
				GOWL_DATADIR "/gowl",
				NULL
			};
			g_autofree gchar *user_dir = NULL;
			gint i;

			user_dir = g_build_filename(
				g_get_user_config_dir(), "gowl", NULL);
			search_dirs[1] = user_dir;

			for (i = 0; search_dirs[i] != NULL; i++) {
				g_autofree gchar *candidate = NULL;

				candidate = g_build_filename(
					search_dirs[i], "config.c", NULL);
				if (g_file_test(candidate, G_FILE_TEST_EXISTS)) {
					c_source = g_steal_pointer(&candidate);
					break;
				}
			}
		}

		if (c_source == NULL) {
			g_printerr("No config.c found\n");
			ret = 1;
			goto cleanup;
		}

		g_print("Compiling %s -> %s\n", c_source, so_path);

		if (!gowl_config_compiler_compile(compiler,
		        c_source, so_path, &error)) {
			g_printerr("Compile failed: %s\n", error->message);
			g_clear_error(&error);
			ret = 1;
		} else {
			g_print("OK\n");
		}

		goto cleanup;
	}

	/* Initialize logging (stderr first, re-init after config load) */
	gowl_log_init(debug_mode ? "debug" : "warning", NULL, FALSE);

	g_message("gowl %s starting...", GOWL_VERSION_STRING);

	/* Load configuration */
	config = gowl_config_new();
	gowl_config = config;

	if (config_path != NULL) {
		if (!gowl_config_load_yaml(config, config_path, &error)) {
			g_warning("Failed to load config from %s: %s",
				config_path, error->message);
			g_clear_error(&error);
		}
	} else {
		if (!gowl_config_load_yaml_from_search_path(config, &error)) {
			g_debug("No YAML config found, using defaults: %s",
				error->message);
			g_clear_error(&error);
		}
	}

	/* Load C config if available */
	if (!no_c_config) {
		g_autoptr(GowlConfigCompiler) compiler = NULL;
		g_autofree gchar *c_source = NULL;
		g_autofree gchar *so_path = NULL;

		compiler = gowl_config_compiler_new();
		so_path  = gowl_config_compiler_get_cache_path(compiler);

		/* Search for config.c in the same paths as YAML config */
		if (c_config_path != NULL) {
			c_source = g_strdup(c_config_path);
		} else {
			const gchar *search_dirs[] = {
				"./data",
				NULL, /* filled in below: ~/.config/gowl */
				GOWL_SYSCONFDIR "/gowl",
				GOWL_DATADIR "/gowl",
				NULL
			};
			g_autofree gchar *user_dir = NULL;
			gint i;

			user_dir = g_build_filename(
				g_get_user_config_dir(), "gowl", NULL);
			search_dirs[1] = user_dir;

			for (i = 0; search_dirs[i] != NULL; i++) {
				g_autofree gchar *candidate = NULL;

				candidate = g_build_filename(
					search_dirs[i], "config.c", NULL);
				if (g_file_test(candidate, G_FILE_TEST_EXISTS)) {
					c_source = g_steal_pointer(&candidate);
					break;
				}
			}
		}

		/* Compile and load if a config.c was found */
		if (c_source != NULL) {
			g_debug("Compiling C config: %s -> %s",
			        c_source, so_path);

			if (!gowl_config_compiler_compile(compiler,
			        c_source, so_path, &error)) {
				g_warning("C config compile failed: %s",
				          error->message);
				g_clear_error(&error);
			} else if (!gowl_config_compiler_load_and_apply(
			               compiler, so_path, &error)) {
				g_warning("C config load failed: %s",
				          error->message);
				g_clear_error(&error);
			} else {
				g_debug("C config loaded successfully");
			}
		}
	}

	/* Re-initialize logging with config values (file + level).
	 * In debug mode: force debug level and truncate the log file
	 * so each session starts with a clean log.
	 */
	gowl_log_init(
		debug_mode ? "debug" : gowl_config_get_log_level(config),
		gowl_config_get_log_file(config),
		debug_mode);

	/* Initialize module manager */
	module_mgr = gowl_module_manager_new();

	/* Create compositor and wire up config + module manager */
	compositor = gowl_compositor_new();
	gowl_compositor = compositor;
	gowl_compositor_set_config(compositor, config);
	gowl_compositor_set_module_manager(compositor, module_mgr);

	/* Create and start IPC server */
	ipc = gowl_ipc_new(NULL);
	if (!gowl_ipc_start(ipc, &error)) {
		g_warning("Failed to start IPC: %s", error->message);
		g_clear_error(&error);
		g_clear_object(&ipc);
	} else {
		gowl_compositor_set_ipc(compositor, ipc);
	}

	/* Start compositor */
	if (!gowl_compositor_start(compositor, &error)) {
		g_printerr("Failed to start compositor: %s\n", error->message);
		g_error_free(error);
		ret = 1;
		goto cleanup;
	}

	/* Dispatch startup hooks */
	gowl_module_manager_dispatch_startup(module_mgr, compositor);

	/* Run startup command if specified */
	if (startup_cmd != NULL) {
		gint child_pid;

		if (!g_spawn_command_line_async(startup_cmd, &error)) {
			g_warning("Failed to run startup command '%s': %s",
				startup_cmd, error->message);
			g_clear_error(&error);
		} else {
			(void)child_pid;
		}
	}

	/* Run the main event loop */
	gowl_compositor_run(compositor);

	/* Dispatch shutdown hooks */
	gowl_module_manager_dispatch_shutdown(module_mgr, compositor);

cleanup:
	if (ipc != NULL)
		gowl_ipc_stop(ipc);
	g_clear_object(&ipc);
	g_clear_object(&compositor);
	g_clear_object(&module_mgr);
	g_clear_object(&config);
	g_free(config_path);
	g_free(c_config_path);
	g_free(startup_cmd);

	return ret;
}
