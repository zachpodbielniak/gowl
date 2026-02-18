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

#include "gowlbar-app.h"
#include "gowlbar-config.h"
#include "gowlbar-config-compiler.h"

#include <glib.h>
#include <stdio.h>
#include <stdlib.h>

#define GOWLBAR_VERSION GOWL_VERSION

/*
 * Global exports for C config .so to reference.
 * These are resolved at dlopen time by the C config module.
 */
GowlbarApp    *gowlbar_app    = NULL;
GowlbarConfig *gowlbar_config = NULL;

int
main(int argc, char *argv[])
{
	gboolean show_version = FALSE;
	gboolean debug_mode = FALSE;
	gboolean recompile = FALSE;
	gboolean gen_yaml = FALSE;
	gboolean gen_c = FALSE;
	gboolean no_yaml = FALSE;
	gboolean no_c = FALSE;
	gchar *config_path = NULL;
	gchar *c_config_path = NULL;
	GError *error = NULL;
	int ret = 0;

	GOptionEntry entries[] = {
		{ "version", 'v', 0, G_OPTION_ARG_NONE, &show_version,
			"Show version information", NULL },
		{ "debug", 'd', 0, G_OPTION_ARG_NONE, &debug_mode,
			"Enable debug logging", NULL },
		{ "config", 0, 0, G_OPTION_ARG_FILENAME, &config_path,
			"Override YAML config path", "PATH" },
		{ "c-config", 0, 0, G_OPTION_ARG_FILENAME, &c_config_path,
			"Override C config path", "PATH" },
		{ "no-yaml-config", 0, 0, G_OPTION_ARG_NONE, &no_yaml,
			"Skip YAML config loading", NULL },
		{ "no-c-config", 0, 0, G_OPTION_ARG_NONE, &no_c,
			"Skip C config compilation/loading", NULL },
		{ "recompile", 0, 0, G_OPTION_ARG_NONE, &recompile,
			"Compile C config and exit", NULL },
		{ "generate-yaml-config", 0, 0, G_OPTION_ARG_NONE, &gen_yaml,
			"Print default YAML config to stdout and exit", NULL },
		{ "generate-c-config", 0, 0, G_OPTION_ARG_NONE, &gen_c,
			"Print default C config template to stdout and exit", NULL },
		{ NULL }
	};

	GOptionContext *opt_ctx;

	opt_ctx = g_option_context_new("- GObject Wayland Status Bar");
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
		g_print("gowlbar %s\n", GOWLBAR_VERSION);
		ret = 0;
		goto cleanup;
	}

	/* Set up logging */
	if (debug_mode) {
		g_setenv("G_MESSAGES_DEBUG", "all", TRUE);
	}

	/* Handle --generate-yaml-config */
	if (gen_yaml) {
		g_autoptr(GowlbarConfig) cfg = gowlbar_config_new();
		g_autofree gchar *yaml = gowlbar_config_generate_yaml(cfg);
		g_print("%s", yaml);
		ret = 0;
		goto cleanup;
	}

	/* Handle --generate-c-config */
	if (gen_c) {
		g_autofree gchar *c_content = NULL;
		const gchar *example_path = "data/example-bar.c";

		if (g_file_get_contents(example_path, &c_content, NULL, NULL)) {
			g_print("%s", c_content);
		} else {
			/* Fall back to a minimal template */
			g_print("/* gowlbar C config */\n"
			        "#include <glib-object.h>\n"
			        "#include <gmodule.h>\n\n"
			        "extern GObject *gowlbar_app;\n"
			        "extern GObject *gowlbar_config;\n\n"
			        "G_MODULE_EXPORT gboolean\n"
			        "gowlbar_config_init(void)\n"
			        "{\n"
			        "\tg_object_set(gowlbar_config,\n"
			        "\t\t\"height\", 24,\n"
			        "\t\t\"background\", \"#222222\",\n"
			        "\t\tNULL);\n"
			        "\treturn TRUE;\n"
			        "}\n");
		}
		ret = 0;
		goto cleanup;
	}

	g_debug("gowlbar %s starting...", GOWLBAR_VERSION);

	/* Create config and app objects */
	gowlbar_config = gowlbar_config_new();
	gowlbar_app = gowlbar_app_new();

	/* Step 1: Load YAML config (unless --no-yaml-config) */
	if (!no_yaml) {
		if (config_path != NULL) {
			/* Explicit path */
			if (!gowlbar_config_load_yaml(gowlbar_config,
			                               config_path,
			                               &error)) {
				g_printerr("gowlbar: failed to load config '%s': %s\n",
				           config_path, error->message);
				g_clear_error(&error);
			}
		} else {
			/* Search path */
			if (!gowlbar_config_load_yaml_from_search_path(
					gowlbar_config, &error)) {
				g_printerr("gowlbar: config error: %s\n",
				           error->message);
				g_clear_error(&error);
			}
		}
	}

	/* Step 2: Compile and load C config (unless --no-c-config) */
	if (!no_c) {
		g_autoptr(GowlbarConfigCompiler) compiler = NULL;
		g_autofree gchar *source = NULL;
		g_autofree gchar *so_path = NULL;

		compiler = gowlbar_config_compiler_new(&error);
		if (compiler == NULL) {
			g_warning("gowlbar: cannot create config compiler: %s",
			          error->message);
			g_clear_error(&error);
			if (recompile) {
				ret = 1;
				goto cleanup;
			}
			goto skip_c_config;
		}

		/* Determine source path */
		if (c_config_path != NULL)
			source = g_strdup(c_config_path);
		else
			source = gowlbar_config_compiler_find_config(compiler);

		if (source != NULL) {
			/* Handle --recompile: force compile and exit */
			if (recompile) {
				g_print("Compiling %s ...\n", source);
				so_path = gowlbar_config_compiler_compile(
					compiler, source, TRUE, &error);
				if (so_path == NULL) {
					g_printerr("gowlbar: compile failed: %s\n",
					           error->message);
					ret = 1;
				} else {
					g_print("Compilation successful: %s\n", so_path);
				}
				goto cleanup;
			}

			/* Normal flow: compile (crispy handles caching) */
			so_path = gowlbar_config_compiler_compile(
				compiler, source, FALSE, &error);
			if (so_path == NULL) {
				g_warning("gowlbar: C config compile failed: %s",
				          error->message);
				g_clear_error(&error);
			} else {
				/* Load the compiled .so */
				if (!gowlbar_config_compiler_load_and_apply(
						compiler, so_path, &error)) {
					g_warning("gowlbar: C config load failed: %s",
					          error->message);
					g_clear_error(&error);
				}
			}
		} else if (recompile) {
			g_printerr("gowlbar: no C config source found\n");
			ret = 1;
			goto cleanup;
		}
	} else if (recompile) {
		g_printerr("gowlbar: --recompile and --no-c-config are "
		           "mutually exclusive\n");
		ret = 1;
		goto cleanup;
	}
skip_c_config:

	/* Apply config to app */
	gowlbar_app_set_config(gowlbar_app, gowlbar_config);

	/* Run the bar */
	if (!gowlbar_app_run(gowlbar_app, &error)) {
		g_printerr("gowlbar: %s\n", error->message);
		g_error_free(error);
		error = NULL;
		ret = 1;
	}

cleanup:
	g_clear_object(&gowlbar_app);
	g_clear_object(&gowlbar_config);
	g_free(config_path);
	g_free(c_config_path);

	return ret;
}
