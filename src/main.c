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
#include "config/gowl-keybind.h"
#include "ipc/gowl-ipc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Global pointers exported for C config shared objects.
 * The user's config.c declares these as extern and accesses
 * them from gowl_config_init() at dlopen time.
 */
GowlCompositor *gowl_compositor = NULL;
GowlConfig     *gowl_config = NULL;

/**
 * GowlBuiltinModule:
 *
 * Static registry of known modules for --list-modules and
 * --modules CSV generation.  Each entry maps a module name
 * to its description and default YAML config snippet.
 */
typedef struct {
	const gchar *name;
	const gchar *description;
	const gchar *yaml_snippet;
	const gchar *c_snippet;
} GowlBuiltinModule;

static const GowlBuiltinModule builtin_modules[] = {
	{
		"autostart",
		"Spawn configured commands on compositor startup",
		"  autostart:\n"
		"    enabled: true\n"
		"    commands:\n"
		"      - \"gowlbar\"\n",
		" *   - autostart: spawn commands on startup\n"
	},
	{
		"centeredmaster",
		"Centered master tiling layout",
		"  centeredmaster:\n"
		"    enabled: true\n",
		" *   - centeredmaster: |M| layout\n"
	},
	{
		"fibonacci",
		"Fibonacci spiral tiling layout",
		"  fibonacci:\n"
		"    enabled: true\n",
		" *   - fibonacci: [@] layout\n"
	},
	{
		"ipc",
		"Handle IPC commands (ping, version, quit, reload)",
		"  ipc:\n"
		"    enabled: true\n",
		" *   - ipc: handle IPC commands\n"
	},
	{
		"movestack",
		"Move clients up/down in the tiling stack",
		"  movestack:\n"
		"    enabled: true\n",
		" *   - movestack: reorder clients in stack\n"
	},
	{
		"pertag",
		"Per-tag layout state (layout, mfact, nmaster)",
		"  pertag:\n"
		"    enabled: true\n",
		" *   - pertag: per-tag layout state\n"
	},
	{
		"scratchpad",
		"Named scratchpad window management",
		"  scratchpad:\n"
		"    enabled: true\n",
		" *   - scratchpad: named scratchpad windows\n"
	},
	{
		"swallow",
		"Window swallowing for terminal-spawned applications",
		"  swallow:\n"
		"    enabled: true\n"
		"    terminals:\n"
		"      - \"st-256color\"\n"
		"      - \"kitty\"\n"
		"      - \"foot\"\n",
		" *   - swallow: terminal window swallowing\n"
	},
	{
		"vanitygaps",
		"Configurable inner and outer gaps for tiling layouts",
		"  vanitygaps:\n"
		"    enabled: true\n"
		"    inner-h: 5\n"
		"    inner-v: 5\n"
		"    outer-h: 5\n"
		"    outer-v: 5\n",
		" *   - vanitygaps: configurable gaps\n"
	},
	{
		"copyhighlight",
		"Sync primary selection to clipboard on change",
		"  copyhighlight:\n"
		"    enabled: true\n",
		" *   - copyhighlight: primary selection to clipboard\n"
	},
	{
		"mcp",
		"MCP server for AI compositor control (requires MCP=1 build)",
		"  mcp:\n"
		"    enabled: true\n"
		"    transport-stdio: true\n"
		"    transport-http: false\n",
		" *   - mcp: AI compositor control via MCP protocol\n"
	},
	{ NULL, NULL, NULL, NULL }
};

/* Default YAML config content for --generate-yaml-config.
 *
 * NOTE: The config parser expects a FLAT mapping — all property keys
 * (border-width, log-level, keybinds, etc.) must be at the root level.
 * Nested sections like "compositor:" are NOT supported by the parser;
 * they are used here only as YAML comments for human organisation.
 * Key names MUST use hyphens (log-level, not log_level).
 */
static const gchar *default_yaml_config =
	"# Gowl Default Configuration\n"
	"# Search order: ./data/ > ~/.config/gowl/ > /etc/gowl/ > /usr/local/gowl/\n"
	"#\n"
	"# Generate this file with: gowl --generate-yaml-config > ~/.config/gowl/config.yaml\n"
	"\n"
	"# Compositor settings\n"
	"log-level: \"warning\"\n"
	"log-file: \"~/.config/gowl/gowl.log\"\n"
	"repeat-rate: 25\n"
	"repeat-delay: 600\n"
	"terminal: \"gst\"\n"
	"menu: \"bemenu-run\"\n"
	"sloppyfocus: true\n"
	"\n"
	"# Appearance\n"
	"border-width: 2\n"
	"border-color-focus: \"#005577\"\n"
	"border-color-unfocus: \"#444444\"\n"
	"border-color-urgent: \"#ff0000\"\n"
	"\n"
	"# Layout\n"
	"mfact: 0.55\n"
	"nmaster: 1\n"
	"\n"
	"# Tags\n"
	"tag-count: 9\n"
	"\n"
	"# Keybinds\n"
	"keybinds:\n"
	"  # Launch applications\n"
	"  \"Super+Return\": { action: spawn, arg: \"gst\" }\n"
	"  \"Super+p\": { action: spawn, arg: \"bemenu-run\" }\n"
	"\n"
	"  # Client management\n"
	"  \"Super+Shift+c\": { action: kill_client }\n"
	"  \"Super+space\": { action: toggle_float }\n"
	"  \"Super+Shift+space\": { action: toggle_fullscreen }\n"
	"  \"Super+Shift+Return\": { action: zoom }\n"
	"\n"
	"  # Focus navigation\n"
	"  \"Super+j\": { action: focus_stack, arg: \"+1\" }\n"
	"  \"Super+k\": { action: focus_stack, arg: \"-1\" }\n"
	"\n"
	"  # Master area adjustment\n"
	"  \"Super+h\": { action: set_mfact, arg: \"-0.05\" }\n"
	"  \"Super+l\": { action: set_mfact, arg: \"+0.05\" }\n"
	"  \"Super+i\": { action: inc_nmaster, arg: \"+1\" }\n"
	"  \"Super+d\": { action: inc_nmaster, arg: \"-1\" }\n"
	"\n"
	"  # Layout selection\n"
	"  \"Super+t\": { action: set_layout, arg: \"tile\" }\n"
	"  \"Super+f\": { action: set_layout, arg: \"float\" }\n"
	"  \"Super+m\": { action: set_layout, arg: \"monocle\" }\n"
	"\n"
	"  # Multi-monitor\n"
	"  \"Super+comma\": { action: focus_monitor, arg: \"-1\" }\n"
	"  \"Super+period\": { action: focus_monitor, arg: \"+1\" }\n"
	"  \"Super+Shift+comma\": { action: move_to_monitor, arg: \"-1\" }\n"
	"  \"Super+Shift+period\": { action: move_to_monitor, arg: \"+1\" }\n"
	"\n"
	"  # View all tags / tag all\n"
	"  \"Super+0\": { action: tag_view, arg: \"0\" }\n"
	"  \"Super+Shift+0\": { action: tag_set, arg: \"0\" }\n"
	"\n"
	"  # Tag switching (Super+1 through Super+9)\n"
	"  \"Super+1\": { action: tag_view, arg: \"1\" }\n"
	"  \"Super+2\": { action: tag_view, arg: \"2\" }\n"
	"  \"Super+3\": { action: tag_view, arg: \"4\" }\n"
	"  \"Super+4\": { action: tag_view, arg: \"8\" }\n"
	"  \"Super+5\": { action: tag_view, arg: \"16\" }\n"
	"  \"Super+6\": { action: tag_view, arg: \"32\" }\n"
	"  \"Super+7\": { action: tag_view, arg: \"64\" }\n"
	"  \"Super+8\": { action: tag_view, arg: \"128\" }\n"
	"  \"Super+9\": { action: tag_view, arg: \"256\" }\n"
	"\n"
	"  # Tag assignment (Super+Shift+1 through Super+Shift+9)\n"
	"  \"Super+Shift+1\": { action: tag_set, arg: \"1\" }\n"
	"  \"Super+Shift+2\": { action: tag_set, arg: \"2\" }\n"
	"  \"Super+Shift+3\": { action: tag_set, arg: \"4\" }\n"
	"  \"Super+Shift+4\": { action: tag_set, arg: \"8\" }\n"
	"  \"Super+Shift+5\": { action: tag_set, arg: \"16\" }\n"
	"  \"Super+Shift+6\": { action: tag_set, arg: \"32\" }\n"
	"  \"Super+Shift+7\": { action: tag_set, arg: \"64\" }\n"
	"  \"Super+Shift+8\": { action: tag_set, arg: \"128\" }\n"
	"  \"Super+Shift+9\": { action: tag_set, arg: \"256\" }\n"
	"\n"
	"  # Session\n"
	"  \"Super+Shift+q\": { action: quit }\n"
	"  \"Super+Shift+r\": { action: reload_config }\n"
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

/**
 * find_builtin_module:
 * @name: module name to look up
 *
 * Searches the builtin_modules registry for a module by name.
 *
 * Returns: pointer to the entry, or %NULL if not found
 */
static const GowlBuiltinModule *
find_builtin_module(const gchar *name)
{
	gint i;

	for (i = 0; builtin_modules[i].name != NULL; i++) {
		if (strcmp(builtin_modules[i].name, name) == 0)
			return &builtin_modules[i];
	}

	return NULL;
}

/**
 * parse_modules_csv:
 * @csv: comma-separated module names (e.g. "vanitygaps,autostart")
 * @out_count: (out): number of parsed names
 *
 * Splits a CSV string into an array of trimmed module names.
 * Caller must free the returned array with g_strfreev().
 *
 * Returns: %NULL-terminated array of module name strings
 */
static gchar **
parse_modules_csv(const gchar *csv, gint *out_count)
{
	gchar **parts;
	gint i;
	gint count;

	parts = g_strsplit(csv, ",", -1);
	count = 0;

	/* Trim whitespace from each entry */
	for (i = 0; parts[i] != NULL; i++) {
		g_strstrip(parts[i]);
		if (parts[i][0] != '\0')
			count++;
	}

	if (out_count != NULL)
		*out_count = count;

	return parts;
}

/**
 * generate_yaml_with_modules:
 * @modules_csv: comma-separated module names
 *
 * Prints the default YAML config with the modules section
 * replaced by entries for the specified modules (all enabled).
 */
static void
generate_yaml_with_modules(const gchar *modules_csv)
{
	g_autofree gchar *base = NULL;
	gchar **mod_names;
	gint count;
	gchar *modules_pos;
	gint i;

	mod_names = parse_modules_csv(modules_csv, &count);

	/* Print the base config up to the modules: section */
	base = g_strdup(default_yaml_config);
	modules_pos = strstr(base, "modules:\n");
	if (modules_pos != NULL)
		*modules_pos = '\0';

	g_print("%s", base);
	g_print("modules:\n");

	/* Emit each requested module's YAML snippet */
	for (i = 0; mod_names[i] != NULL; i++) {
		const GowlBuiltinModule *bmod;

		if (mod_names[i][0] == '\0')
			continue;

		bmod = find_builtin_module(mod_names[i]);
		if (bmod != NULL) {
			g_print("%s", bmod->yaml_snippet);
		} else {
			g_printerr("warning: unknown module '%s'\n",
			           mod_names[i]);
		}
	}

	g_strfreev(mod_names);
}

/**
 * generate_c_with_modules:
 * @modules_csv: comma-separated module names
 *
 * Prints the default C config template with comments listing
 * which modules are expected to be loaded.
 */
static void
generate_c_with_modules(const gchar *modules_csv)
{
	gchar **mod_names;
	gint i;

	mod_names = parse_modules_csv(modules_csv, NULL);

	g_print(
		"/*\n"
		" * gowl user configuration\n"
		" *\n"
		" * Compile args are taken from the CRISPY_PARAMS define if present,\n"
		" * otherwise pkg-config is used. This file is compiled to a .so and\n"
		" * loaded at startup. On compile failure, defaults are used.\n"
		" *\n"
		" * Optional build args override:\n"
		" * #define CRISPY_PARAMS \"-I/custom/path\"\n"
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
		" *\n"
		" * Modules enabled via YAML config:\n");

	for (i = 0; mod_names[i] != NULL; i++) {
		const GowlBuiltinModule *bmod;

		if (mod_names[i][0] == '\0')
			continue;

		bmod = find_builtin_module(mod_names[i]);
		if (bmod != NULL)
			g_print("%s", bmod->c_snippet);
	}

	g_print(
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
		"}\n");

	g_strfreev(mod_names);
}

/**
 * print_module_list:
 *
 * Prints all known modules with a short summary.
 */
static void
print_module_list(void)
{
	gint i;

	g_print("Available gowl modules:\n\n");
	g_print("  %-18s %s\n", "MODULE", "DESCRIPTION");
	g_print("  %-18s %s\n", "------", "-----------");

	for (i = 0; builtin_modules[i].name != NULL; i++) {
		g_print("  %-18s %s\n",
		        builtin_modules[i].name,
		        builtin_modules[i].description);
	}

	g_print("\nUse --modules MODULE1,MODULE2 with --generate-yaml-config\n"
	        "or --generate-c-config to include modules in generated config.\n");
}

/* Default C config template for --generate-c-config */
static const gchar *default_c_config =
	"/*\n"
	" * gowl user configuration\n"
	" *\n"
	" * Compile args are taken from the CRISPY_PARAMS define if present,\n"
	" * otherwise pkg-config is used. This file is compiled to a .so and\n"
	" * loaded at startup. On compile failure, defaults are used.\n"
	" *\n"
	" * Optional build args override:\n"
	" * #define CRISPY_PARAMS \"-I/custom/path\"\n"
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
	gboolean list_modules = FALSE;
	gboolean no_c_config = FALSE;
	gboolean no_yaml_config = FALSE;
	gboolean recompile = FALSE;
	gchar *config_path = NULL;
	gchar *c_config_path = NULL;
	gchar *startup_cmd = NULL;
	gchar *modules_csv = NULL;
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
		{ "modules", 0, 0, G_OPTION_ARG_STRING, &modules_csv,
			"Comma-separated modules for config generation",
			"MOD1,MOD2" },
		{ "list-modules", 0, 0, G_OPTION_ARG_NONE, &list_modules,
			"List available modules with descriptions", NULL },
		{ "config", 0, 0, G_OPTION_ARG_FILENAME, &config_path,
			"Override YAML config path", "PATH" },
		{ "c-config", 0, 0, G_OPTION_ARG_FILENAME, &c_config_path,
			"Override C config path", "PATH" },
		{ "no-c-config", 0, 0, G_OPTION_ARG_NONE, &no_c_config,
			"Skip C config compilation", NULL },
		{ "no-yaml-config", 0, 0, G_OPTION_ARG_NONE, &no_yaml_config,
			"Skip YAML config loading", NULL },
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

	/* Handle --list-modules */
	if (list_modules) {
		print_module_list();
		ret = 0;
		goto cleanup;
	}

	/* Handle --generate-yaml-config */
	if (generate_yaml) {
		if (modules_csv != NULL)
			generate_yaml_with_modules(modules_csv);
		else
			g_print("%s", default_yaml_config);
		ret = 0;
		goto cleanup;
	}

	/* Handle --generate-c-config */
	if (generate_c) {
		if (modules_csv != NULL)
			generate_c_with_modules(modules_csv);
		else
			g_print("%s", default_c_config);
		ret = 0;
		goto cleanup;
	}

	/* Handle --recompile */
	if (recompile) {
		g_autoptr(GowlConfigCompiler) compiler = NULL;
		g_autofree gchar *c_source = NULL;
		g_autofree gchar *so_path = NULL;

		compiler = gowl_config_compiler_new(&error);
		if (compiler == NULL) {
			g_printerr("Cannot create compiler: %s\n",
			           error->message);
			g_clear_error(&error);
			ret = 1;
			goto cleanup;
		}

		if (c_config_path != NULL) {
			c_source = g_strdup(c_config_path);
		} else {
			c_source = gowl_config_compiler_find_config(compiler);
		}

		if (c_source == NULL) {
			g_printerr("No config.c found\n");
			ret = 1;
			goto cleanup;
		}

		g_print("Compiling %s ...\n", c_source);

		so_path = gowl_config_compiler_compile(compiler,
		              c_source, TRUE, &error);
		if (so_path == NULL) {
			g_printerr("Compile failed: %s\n", error->message);
			g_clear_error(&error);
			ret = 1;
		} else {
			g_print("OK: %s\n", so_path);
		}

		goto cleanup;
	}

	/* Initialize logging (stderr first, re-init after config load) */
	gowl_log_init(debug_mode ? "debug" : "warning", NULL, FALSE);

	g_message("gowl %s starting...", GOWL_VERSION_STRING);

	/* Load configuration */
	config = gowl_config_new();
	gowl_config = config;

	if (!no_yaml_config) {
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
	}

	/* Load C config if available */
	if (!no_c_config) {
		g_autoptr(GowlConfigCompiler) compiler = NULL;
		g_autofree gchar *c_source = NULL;
		g_autofree gchar *so_path = NULL;

		compiler = gowl_config_compiler_new(&error);
		if (compiler == NULL) {
			g_warning("Cannot create C config compiler: %s",
			          error->message);
			g_clear_error(&error);
		} else {
			/* Search for config.c */
			if (c_config_path != NULL) {
				c_source = g_strdup(c_config_path);
			} else {
				c_source = gowl_config_compiler_find_config(
				               compiler);
			}

			/* Compile and load if a config.c was found */
			if (c_source != NULL) {
				so_path = gowl_config_compiler_compile(
				              compiler, c_source, FALSE,
				              &error);
				if (so_path == NULL) {
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
	}

	/* Re-initialize logging with config values (file + level).
	 * In debug mode: force debug level and truncate the log file
	 * so each session starts with a clean log.
	 */
	gowl_log_init(
		debug_mode ? "debug" : gowl_config_get_log_level(config),
		gowl_config_get_log_file(config),
		debug_mode);

	/* Initialize module manager and load modules */
	module_mgr = gowl_module_manager_new();

	/*
	 * Load only modules that are enabled in the YAML config.
	 *
	 * Build two search directories (dev first, then installed).
	 * For each module listed as "enabled: true" in the config,
	 * find its .so in the first directory that has it and load it.
	 * This prevents duplicates and respects the enabled flag.
	 */
	{
		g_autofree gchar *dev_mod_dir = NULL;
		g_autofree gchar *bin_dir = NULL;
		g_autofree gchar *exe_path = NULL;
		const gchar *search_dirs[3] = { NULL, NULL, NULL };
		gint n_dirs = 0;
		GHashTable *enabled_modules;
		GHashTableIter iter;
		gpointer key, value;

		/* Build ordered list of module search directories */
		exe_path = g_file_read_link("/proc/self/exe", NULL);
		if (exe_path != NULL) {
			bin_dir = g_path_get_dirname(exe_path);
			dev_mod_dir = g_build_filename(bin_dir, "modules", NULL);
			if (g_file_test(dev_mod_dir, G_FILE_TEST_IS_DIR))
				search_dirs[n_dirs++] = dev_mod_dir;
		}
		if (g_file_test(GOWL_MODULEDIR, G_FILE_TEST_IS_DIR))
			search_dirs[n_dirs++] = GOWL_MODULEDIR;

		/* Collect the set of module names with enabled: true */
		enabled_modules = gowl_config_get_all_module_configs(config);

		if (enabled_modules != NULL) {
			g_hash_table_iter_init(&iter, enabled_modules);
			while (g_hash_table_iter_next(&iter, &key, &value)) {
				const gchar *mod_name = (const gchar *)key;
				GHashTable *mod_cfg = (GHashTable *)value;
				const gchar *enabled_str;
				gboolean enabled;
				gboolean loaded;
				gint di;

				/* Check enabled flag; default to false if absent */
				enabled_str = (const gchar *)g_hash_table_lookup(
					mod_cfg, "enabled");
				enabled = (enabled_str != NULL &&
				           g_ascii_strcasecmp(enabled_str, "true") == 0);

				if (!enabled) {
					g_debug("Module '%s' is disabled, skipping", mod_name);
					continue;
				}

				/* Try each search directory in priority order */
				loaded = FALSE;
				for (di = 0; di < n_dirs && !loaded; di++) {
					g_autofree gchar *so_name = NULL;
					g_autofree gchar *so_path = NULL;
					GError *mod_err = NULL;

					so_name = g_strdup_printf("%s.so", mod_name);
					so_path = g_build_filename(
						search_dirs[di], so_name, NULL);

					if (!g_file_test(so_path, G_FILE_TEST_EXISTS))
						continue;

					if (gowl_module_manager_load_module(
					        module_mgr, so_path, &mod_err)) {
						g_message("Loaded module '%s' from %s",
						          mod_name, so_path);
						loaded = TRUE;
					} else {
						g_warning("Failed to load module '%s': %s",
						          so_path, mod_err->message);
						g_error_free(mod_err);
					}
				}

				if (!loaded)
					g_warning("Module '%s' is enabled but .so not found",
					          mod_name);
			}
		}

		/* Activate all loaded (enabled) modules */
		gowl_module_manager_activate_all(module_mgr);
	}

	/* Pass per-module YAML config to each loaded module.
	 * This iterates the actual GowlModule instances (not GowlModuleInfo)
	 * and calls gowl_module_configure() with the matching settings. */
	gowl_module_manager_configure_all(
		module_mgr,
		gowl_config_get_all_module_configs(config));

	/* Create compositor and wire up config + module manager */
	compositor = gowl_compositor_new();
	gowl_compositor = compositor;
	gowl_compositor_set_config(compositor, config);
	gowl_compositor_set_module_manager(compositor, module_mgr);

	/* Dump registered keybinds for diagnostics */
	{
		GArray *kb_arr = gowl_config_get_keybinds(config);
		guint kb_i;

		g_message("Registered %u keybind(s):", kb_arr ? kb_arr->len : 0);
		if (kb_arr != NULL) {
			for (kb_i = 0; kb_i < kb_arr->len; kb_i++) {
				GowlKeybindEntry *ent;
				g_autofree gchar *name = NULL;

				ent  = &g_array_index(kb_arr, GowlKeybindEntry, kb_i);
				name = gowl_keybind_to_string(ent->modifiers, ent->keysym);
				g_message("  [%u] %s -> action=%d mods=0x%x sym=0x%x arg=%s",
				          kb_i, name ? name : "???",
				          ent->action, ent->modifiers, ent->keysym,
				          ent->arg ? ent->arg : "(null)");
			}
		}
	}

	/* Start compositor (creates wl_display and event loop) */
	if (!gowl_compositor_start(compositor, &error)) {
		g_printerr("Failed to start compositor: %s\n", error->message);
		g_error_free(error);
		ret = 1;
		goto cleanup;
	}

	/* Create and start IPC server (needs compositor's event loop) */
	ipc = gowl_ipc_new(NULL);
	if (!gowl_ipc_start(ipc,
	                     gowl_compositor_get_event_loop(compositor),
	                     &error)) {
		g_warning("Failed to start IPC: %s", error->message);
		g_clear_error(&error);
		g_clear_object(&ipc);
	} else {
		gowl_compositor_set_ipc(compositor, ipc);
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
	g_free(modules_csv);

	return ret;
}
