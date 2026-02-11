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

#include "gowl-config-compiler.h"

#include <glib.h>
#include <gio/gio.h>
#include <gmodule.h>
#include <string.h>

/**
 * GowlConfigCompiler:
 *
 * Compiles a user-written C configuration file into a shared object
 * and loads it at runtime.  The config source may define
 * GOWL_BUILD_ARGS to pass extra compiler flags (e.g. additional
 * pkg-config packages).  The compiled .so must export a
 * `gowl_config_init` symbol that the compositor calls to apply
 * the configuration.
 */
struct _GowlConfigCompiler {
	GObject  parent_instance;

	gchar   *gcc_path;
	gchar   *cache_dir;
};

G_DEFINE_FINAL_TYPE(GowlConfigCompiler, gowl_config_compiler, G_TYPE_OBJECT)

/* --- GObject lifecycle --- */

static void
gowl_config_compiler_finalize(GObject *object)
{
	GowlConfigCompiler *self;

	self = GOWL_CONFIG_COMPILER(object);

	g_free(self->gcc_path);
	g_free(self->cache_dir);

	G_OBJECT_CLASS(gowl_config_compiler_parent_class)->finalize(object);
}

/* --- class / instance init --- */

static void
gowl_config_compiler_class_init(GowlConfigCompilerClass *klass)
{
	GObjectClass *object_class;

	object_class = G_OBJECT_CLASS(klass);

	object_class->finalize = gowl_config_compiler_finalize;
}

static void
gowl_config_compiler_init(GowlConfigCompiler *self)
{
	self->gcc_path  = g_find_program_in_path("gcc");
	self->cache_dir = g_build_filename(g_get_user_cache_dir(), "gowl", NULL);
}

/* --- Internal helpers --- */

/**
 * extract_build_args:
 * @source_content: the full text of the config source file
 *
 * Scans the source for a line matching `#define GOWL_BUILD_ARGS ...`
 * and extracts the value portion.  Leading/trailing whitespace and
 * surrounding double-quotes are stripped from the result.
 *
 * Returns: (transfer full): the extracted args string, or an empty
 *          string if no GOWL_BUILD_ARGS define was found
 */
static gchar *
extract_build_args(const gchar *source_content)
{
	const gchar *line_start;
	const gchar *pos;
	const gchar *value_start;
	const gchar *line_end;
	gchar *value;

	/* walk through the source looking for the define */
	pos = source_content;
	while (pos != NULL && *pos != '\0') {
		line_start = pos;

		/* find end of this line */
		line_end = strchr(pos, '\n');
		if (line_end == NULL)
			line_end = pos + strlen(pos);

		/* check if this line starts with #define GOWL_BUILD_ARGS */
		if (g_str_has_prefix(line_start, "#define GOWL_BUILD_ARGS")) {
			value_start = line_start + strlen("#define GOWL_BUILD_ARGS");

			/* skip whitespace after the macro name */
			while (value_start < line_end && g_ascii_isspace(*value_start))
				value_start++;

			value = g_strndup(value_start, (gsize)(line_end - value_start));
			g_strstrip(value);

			/* strip surrounding quotes if present */
			if (strlen(value) >= 2 &&
			    value[0] == '"' &&
			    value[strlen(value) - 1] == '"') {
				gchar *unquoted;

				unquoted = g_strndup(value + 1, strlen(value) - 2);
				g_free(value);
				value = unquoted;
			}

			return value;
		}

		/* advance to next line */
		if (*line_end == '\n')
			pos = line_end + 1;
		else
			break;
	}

	return g_strdup("");
}

/* --- Public API --- */

/**
 * gowl_config_compiler_new:
 *
 * Creates a new #GowlConfigCompiler.  The gcc binary is located
 * via g_find_program_in_path() and the cache directory is set to
 * $XDG_CACHE_HOME/gowl.
 *
 * Returns: (transfer full): a newly allocated #GowlConfigCompiler
 */
GowlConfigCompiler *
gowl_config_compiler_new(void)
{
	return (GowlConfigCompiler *)g_object_new(GOWL_TYPE_CONFIG_COMPILER, NULL);
}

/**
 * gowl_config_compiler_compile:
 * @self: a #GowlConfigCompiler
 * @source_path: path to the C configuration source file
 * @output_path: path where the compiled .so should be written
 * @error: (nullable): return location for a #GError, or %NULL
 *
 * Reads the source file, scans for an optional GOWL_BUILD_ARGS
 * define, and invokes gcc to compile the source into a shared
 * object.  The compilation uses -std=gnu89 -shared -fPIC and
 * links against glib-2.0, gobject-2.0, and gmodule-2.0.
 *
 * Returns: %TRUE on success, %FALSE on error
 */
gboolean
gowl_config_compiler_compile(
	GowlConfigCompiler  *self,
	const gchar         *source_path,
	const gchar         *output_path,
	GError             **error
){
	g_autofree gchar *source_content = NULL;
	g_autofree gchar *extra_args = NULL;
	g_autofree gchar *cmd = NULL;
	g_autofree gchar *stderr_output = NULL;
	gint exit_status;
	gboolean ok;

	g_return_val_if_fail(GOWL_IS_CONFIG_COMPILER(self), FALSE);
	g_return_val_if_fail(source_path != NULL, FALSE);
	g_return_val_if_fail(output_path != NULL, FALSE);

	/* verify we found gcc */
	if (self->gcc_path == NULL) {
		g_set_error_literal(error,
		                    G_IO_ERROR,
		                    G_IO_ERROR_NOT_FOUND,
		                    "gcc not found in PATH");
		return FALSE;
	}

	/* read the source file */
	if (!g_file_get_contents(source_path, &source_content, NULL, error))
		return FALSE;

	/* extract optional build arguments from the source */
	extra_args = extract_build_args(source_content);

	/* build the compilation command */
	cmd = g_strdup_printf(
		"%s -std=gnu89 -shared -fPIC %s -o %s %s "
		"$(pkg-config --cflags --libs glib-2.0 gobject-2.0 gmodule-2.0)",
		self->gcc_path,
		extra_args,
		output_path,
		source_path);

	/* execute the compiler */
	ok = g_spawn_command_line_sync(cmd,
	                               NULL,
	                               &stderr_output,
	                               &exit_status,
	                               error);
	if (!ok)
		return FALSE;

	if (!g_spawn_check_exit_status(exit_status, NULL)) {
		g_set_error(error,
		            G_IO_ERROR,
		            G_IO_ERROR_FAILED,
		            "gcc compilation failed:\n%s",
		            stderr_output != NULL ? stderr_output : "(no output)");
		return FALSE;
	}

	return TRUE;
}

/**
 * gowl_config_compiler_get_cache_path:
 * @self: a #GowlConfigCompiler
 *
 * Returns the default path for the compiled config shared object.
 * Creates the cache directory if it does not exist.
 *
 * Returns: (transfer full): a newly allocated path string; free
 *          with g_free()
 */
gchar *
gowl_config_compiler_get_cache_path(GowlConfigCompiler *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG_COMPILER(self), NULL);

	/* ensure the cache directory exists */
	g_mkdir_with_parents(self->cache_dir, 0755);

	return g_build_filename(self->cache_dir, "config.so", NULL);
}

/**
 * gowl_config_compiler_load_and_apply:
 * @self: a #GowlConfigCompiler
 * @so_path: path to the compiled shared object
 * @error: (nullable): return location for a #GError, or %NULL
 *
 * Opens the shared object at @so_path, looks up the
 * `gowl_config_init` symbol, and calls it.  The init function
 * is expected to have the signature `gboolean gowl_config_init(void)`
 * and returns %TRUE on success.
 *
 * Returns: %TRUE if the config was loaded and applied successfully,
 *          %FALSE on error
 */
gboolean
gowl_config_compiler_load_and_apply(
	GowlConfigCompiler  *self,
	const gchar         *so_path,
	GError             **error
){
	GModule *module;
	gpointer symbol;
	gboolean (*config_init_fn)(void);
	gboolean result;

	g_return_val_if_fail(GOWL_IS_CONFIG_COMPILER(self), FALSE);
	g_return_val_if_fail(so_path != NULL, FALSE);

	module = g_module_open(so_path, G_MODULE_BIND_LAZY);
	if (module == NULL) {
		g_set_error(error,
		            G_IO_ERROR,
		            G_IO_ERROR_FAILED,
		            "Failed to open module '%s': %s",
		            so_path,
		            g_module_error());
		return FALSE;
	}

	if (!g_module_symbol(module, "gowl_config_init", &symbol)) {
		g_set_error(error,
		            G_IO_ERROR,
		            G_IO_ERROR_NOT_FOUND,
		            "Symbol 'gowl_config_init' not found in '%s': %s",
		            so_path,
		            g_module_error());
		g_module_close(module);
		return FALSE;
	}

	config_init_fn = (gboolean (*)(void))symbol;
	result = config_init_fn();

	if (!result) {
		g_set_error(error,
		            G_IO_ERROR,
		            G_IO_ERROR_FAILED,
		            "gowl_config_init() returned FALSE in '%s'",
		            so_path);
		g_module_close(module);
		return FALSE;
	}

	/* keep the module open so symbols remain available */

	return TRUE;
}
