/*
 * gowlbar-config-compiler.c - C configuration compiler for gowlbar
 *
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
 * Compiles a user-written C configuration file for gowlbar into a
 * shared object and loads it at runtime.  Uses the crispy library
 * for gcc-based compilation and SHA256 content-hash caching.  The
 * config source may define CRISPY_PARAMS to pass extra compiler
 * flags.  The compiled .so must export a `gowlbar_config_init`
 * symbol that is called to apply the configuration.
 */

#include "gowlbar-config-compiler.h"

#define CRISPY_COMPILATION
#include <crispy.h>

#include <glib.h>
#include <gio/gio.h>
#include <gmodule.h>
#include <string.h>

/**
 * GowlbarConfigCompiler:
 *
 * Compiles a user-written C configuration file for gowlbar into a
 * shared object and loads it at runtime.  Uses the crispy library
 * for compilation and SHA256 content-hash caching.  The config
 * source may define CRISPY_PARAMS to pass extra compiler flags.
 * The compiled .so must export a `gowlbar_config_init` symbol.
 */
struct _GowlbarConfigCompiler {
	GObject              parent_instance;

	CrispyGccCompiler   *compiler;   /* crispy compiler backend */
	CrispyFileCache     *cache;      /* crispy file cache (SHA256) */
};

G_DEFINE_FINAL_TYPE(GowlbarConfigCompiler, gowlbar_config_compiler, G_TYPE_OBJECT)

/* --- GObject lifecycle --- */

static void
gowlbar_config_compiler_finalize(GObject *object)
{
	GowlbarConfigCompiler *self;

	self = GOWLBAR_CONFIG_COMPILER(object);

	g_clear_object(&self->compiler);
	g_clear_object(&self->cache);

	G_OBJECT_CLASS(gowlbar_config_compiler_parent_class)->finalize(object);
}

/* --- class / instance init --- */

static void
gowlbar_config_compiler_class_init(GowlbarConfigCompilerClass *klass)
{
	GObjectClass *object_class;

	object_class = G_OBJECT_CLASS(klass);

	object_class->finalize = gowlbar_config_compiler_finalize;
}

static void
gowlbar_config_compiler_init(GowlbarConfigCompiler *self)
{
	/* fields set in _new() after gcc probe */
	self->compiler = NULL;
	self->cache = NULL;
}

/* --- Internal helpers --- */

/**
 * run_pkg_config:
 * @args: arguments to pass to pkg-config
 * @error: (nullable): return location for a #GError, or %NULL
 *
 * Runs pkg-config with the given arguments and captures stdout.
 *
 * Returns: (transfer full): the pkg-config output string, or %NULL on error
 */
static gchar *
run_pkg_config(
	const gchar  *args,
	GError      **error
){
	g_autofree gchar *cmd = NULL;
	g_autofree gchar *stdout_output = NULL;
	g_autofree gchar *stderr_output = NULL;
	gint exit_status;

	cmd = g_strdup_printf("pkg-config %s", args);

	if (!g_spawn_command_line_sync(cmd, &stdout_output, &stderr_output,
	                               &exit_status, error))
		return NULL;

	if (!g_spawn_check_wait_status(exit_status, NULL)) {
		g_set_error(error,
		            G_IO_ERROR,
		            G_IO_ERROR_FAILED,
		            "pkg-config %s failed: %s",
		            args,
		            stderr_output != NULL ? stderr_output : "(no output)");
		return NULL;
	}

	g_strstrip(stdout_output);
	return g_steal_pointer(&stdout_output);
}

/**
 * get_gowl_include_flags:
 *
 * Attempts to get gowl's include flags via pkg-config.  If gowl
 * is not installed, falls back to the compile-time development
 * include path (GOWL_DEV_INCLUDE_DIR).
 *
 * Returns: (transfer full): include flags string; free with g_free()
 */
static gchar *
get_gowl_include_flags(void)
{
	g_autoptr(GError) error = NULL;
	gchar *flags;

	/* try installed gowl first */
	flags = run_pkg_config("--cflags gowl", &error);
	if (flags != NULL)
		return flags;

#ifdef GOWL_DEV_INCLUDE_DIR
	/* fall back to development include paths */
	return g_strdup("-I" GOWL_DEV_INCLUDE_DIR
	                " -I" GOWL_DEV_INCLUDE_DIR "/gowl");
#else
	return g_strdup("");
#endif
}

/**
 * get_yaml_glib_include_flags:
 *
 * Gets include flags for yaml-glib headers in development mode.
 *
 * Returns: (transfer full): include flags string; free with g_free()
 */
static gchar *
get_yaml_glib_include_flags(void)
{
#ifdef GOWL_DEV_INCLUDE_DIR
	g_autofree gchar *build_dir = NULL;
	g_autofree gchar *project_dir = NULL;
	g_autofree gchar *yaml_dir = NULL;

	build_dir = g_path_get_dirname(GOWL_DEV_INCLUDE_DIR);
	project_dir = g_path_get_dirname(build_dir);
	yaml_dir = g_build_filename(project_dir, "deps",
	                            "yaml-glib", "src", NULL);

	if (g_file_test(yaml_dir, G_FILE_TEST_IS_DIR)) {
		return g_strdup_printf("-I%s", yaml_dir);
	}
#endif

	return g_strdup("");
}

/**
 * get_crispy_include_flags:
 *
 * Gets include flags for crispy headers in development mode.
 *
 * Returns: (transfer full): include flags string; free with g_free()
 */
static gchar *
get_crispy_include_flags(void)
{
#ifdef GOWL_DEV_INCLUDE_DIR
	g_autofree gchar *build_dir = NULL;
	g_autofree gchar *project_dir = NULL;
	g_autofree gchar *crispy_dir = NULL;

	build_dir = g_path_get_dirname(GOWL_DEV_INCLUDE_DIR);
	project_dir = g_path_get_dirname(build_dir);
	crispy_dir = g_build_filename(project_dir, "deps",
	                              "crispy", "src", NULL);

	if (g_file_test(crispy_dir, G_FILE_TEST_IS_DIR)) {
		return g_strdup_printf("-I%s", crispy_dir);
	}
#endif

	return g_strdup("");
}

/**
 * get_gowlbar_extra_pkg_flags:
 *
 * Gets the pkg-config flags for dependencies that gowlbar config
 * needs beyond the base glib/gobject/gio/gmodule that crispy provides.
 *
 * Returns: (transfer full): pkg-config flags string; free with g_free()
 */
static gchar *
get_gowlbar_extra_pkg_flags(void)
{
	g_autoptr(GError) error = NULL;
	gchar *flags;

	flags = run_pkg_config(
		"--cflags --libs pangocairo cairo", &error);
	if (flags != NULL)
		return flags;

	g_warning("Failed to get gowlbar extra pkg-config flags: %s",
	          error->message);
	return g_strdup("");
}

/**
 * extract_crispy_params:
 * @source_content: the full text of the config source file
 *
 * Scans the source for a line matching `#define CRISPY_PARAMS "..."`
 * and extracts the quoted value portion.
 *
 * Returns: (transfer full): the extracted params string, or an empty
 *          string if no CRISPY_PARAMS define was found
 */
static gchar *
extract_crispy_params(const gchar *source_content)
{
	const gchar *line_start;
	const gchar *pos;
	const gchar *value_start;
	const gchar *line_end;
	gchar *value;

	pos = source_content;
	while (pos != NULL && *pos != '\0') {
		line_start = pos;

		line_end = strchr(pos, '\n');
		if (line_end == NULL)
			line_end = pos + strlen(pos);

		if (g_str_has_prefix(line_start, "#define CRISPY_PARAMS")) {
			value_start = line_start + strlen("#define CRISPY_PARAMS");

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

		if (*line_end == '\n')
			pos = line_end + 1;
		else
			break;
	}

	return g_strdup("");
}

/**
 * shell_expand:
 * @params: the raw CRISPY_PARAMS value
 * @error: (nullable): return location for a #GError, or %NULL
 *
 * Shell-expands the CRISPY_PARAMS value.
 *
 * Returns: (transfer full): the expanded string, or %NULL on error
 */
static gchar *
shell_expand(
	const gchar  *params,
	GError      **error
){
	g_autofree gchar *cmd = NULL;
	g_autofree gchar *stdout_output = NULL;
	g_autofree gchar *stderr_output = NULL;
	gint exit_status;

	if (params == NULL || params[0] == '\0')
		return g_strdup("");

	cmd = g_strdup_printf("/bin/sh -c \"printf '%%s' %s\"", params);

	if (!g_spawn_command_line_sync(cmd, &stdout_output, &stderr_output,
	                               &exit_status, error))
		return NULL;

	if (!g_spawn_check_wait_status(exit_status, NULL)) {
		g_set_error(error,
		            G_IO_ERROR,
		            G_IO_ERROR_FAILED,
		            "CRISPY_PARAMS expansion failed: %s",
		            stderr_output != NULL ? stderr_output : "(no output)");
		return NULL;
	}

	g_strstrip(stdout_output);
	return g_steal_pointer(&stdout_output);
}

/* --- Public API --- */

/**
 * gowlbar_config_compiler_new:
 * @error: (nullable): return location for a #GError, or %NULL
 *
 * Creates a new #GowlbarConfigCompiler backed by the crispy library.
 * Probes gcc for its version and caches pkg-config output.
 * Sets up SHA256 content-hash caching in $XDG_CACHE_HOME/gowl.
 *
 * Returns: (transfer full) (nullable): a new #GowlbarConfigCompiler,
 *          or %NULL if gcc is not found
 */
GowlbarConfigCompiler *
gowlbar_config_compiler_new(GError **error)
{
	GowlbarConfigCompiler *self;
	g_autofree gchar *cache_dir = NULL;

	self = (GowlbarConfigCompiler *)g_object_new(
		GOWLBAR_TYPE_CONFIG_COMPILER, NULL);

	/* create crispy gcc compiler (probes gcc, caches base flags) */
	self->compiler = crispy_gcc_compiler_new(error);
	if (self->compiler == NULL) {
		g_object_unref(self);
		return NULL;
	}

	/* create crispy file cache in ~/.cache/gowl */
	cache_dir = g_build_filename(g_get_user_cache_dir(), "gowl", NULL);
	self->cache = crispy_file_cache_new_with_dir(cache_dir);

	return self;
}

/**
 * gowlbar_config_compiler_find_config:
 * @self: a #GowlbarConfigCompiler
 *
 * Searches standard paths for a bar C config file.
 *
 * Returns: (transfer full) (nullable): Path to the bar.c,
 *          or %NULL if none found
 */
gchar *
gowlbar_config_compiler_find_config(GowlbarConfigCompiler *self)
{
	gchar *path;

	g_return_val_if_fail(GOWLBAR_IS_CONFIG_COMPILER(self), NULL);

	/* 1. XDG user config */
	path = g_build_filename(g_get_user_config_dir(),
	                        "gowl", "bar.c", NULL);
	if (g_file_test(path, G_FILE_TEST_IS_REGULAR))
		return path;
	g_free(path);

	/* 2. System config (SYSCONFDIR) */
#ifdef GOWL_SYSCONFDIR
	path = g_build_filename(GOWL_SYSCONFDIR, "gowl", "bar.c", NULL);
	if (g_file_test(path, G_FILE_TEST_IS_REGULAR))
		return path;
	g_free(path);
#endif

	/* 3. Shared data (DATADIR) */
#ifdef GOWL_DATADIR
	path = g_build_filename(GOWL_DATADIR, "gowl", "bar.c", NULL);
	if (g_file_test(path, G_FILE_TEST_IS_REGULAR))
		return path;
	g_free(path);
#endif

	/* 4. Development fallback: ./data/bar.c relative to exe */
	{
		g_autofree gchar *exe_path = NULL;
		g_autofree gchar *exe_dir = NULL;

		exe_path = g_file_read_link("/proc/self/exe", NULL);
		if (exe_path != NULL) {
			exe_dir = g_path_get_dirname(exe_path);
			path = g_build_filename(exe_dir, "..", "data",
			                        "bar.c", NULL);
			if (g_file_test(path, G_FILE_TEST_IS_REGULAR))
				return path;
			g_free(path);
		}
	}

	return NULL;
}

/**
 * gowlbar_config_compiler_compile:
 * @self: a #GowlbarConfigCompiler
 * @source_path: path to the C configuration source file
 * @force: if %TRUE, bypass cache and force recompilation
 * @error: (nullable): return location for a #GError, or %NULL
 *
 * Reads the source file, scans for an optional CRISPY_PARAMS
 * define, computes a SHA256 content hash, and compiles to a
 * shared object if no valid cached artifact exists (or if
 * @force is %TRUE).
 *
 * Returns: (transfer full) (nullable): path to the compiled .so,
 *          or %NULL on error.  Free with g_free().
 */
gchar *
gowlbar_config_compiler_compile(
	GowlbarConfigCompiler  *self,
	const gchar            *source_path,
	gboolean                force,
	GError                **error
){
	g_autofree gchar *source_content = NULL;
	g_autofree gchar *raw_params = NULL;
	g_autofree gchar *expanded_params = NULL;
	g_autofree gchar *gowl_flags = NULL;
	g_autofree gchar *yaml_flags = NULL;
	g_autofree gchar *crispy_flags = NULL;
	g_autofree gchar *extra_pkg_flags = NULL;
	g_autofree gchar *extra_flags = NULL;
	g_autofree gchar *hash = NULL;
	gchar *so_path;
	const gchar *compiler_version;

	g_return_val_if_fail(GOWLBAR_IS_CONFIG_COMPILER(self), NULL);
	g_return_val_if_fail(source_path != NULL, NULL);

	/* read the source file */
	if (!g_file_get_contents(source_path, &source_content, NULL, error))
		return NULL;

	/* extract optional CRISPY_PARAMS from the source */
	raw_params = extract_crispy_params(source_content);

	/* shell-expand CRISPY_PARAMS (supports $(pkg-config ...) etc.) */
	expanded_params = shell_expand(raw_params, error);
	if (expanded_params == NULL)
		return NULL;

	/* get include flags */
	gowl_flags = get_gowl_include_flags();
	yaml_flags = get_yaml_glib_include_flags();
	crispy_flags = get_crispy_include_flags();
	extra_pkg_flags = get_gowlbar_extra_pkg_flags();

	/* build the combined extra_flags string */
	extra_flags = g_strdup_printf("%s %s %s %s %s",
	                              gowl_flags,
	                              yaml_flags,
	                              crispy_flags,
	                              extra_pkg_flags,
	                              expanded_params);

	/* compute content hash for caching */
	compiler_version = crispy_compiler_get_version(
		CRISPY_COMPILER(self->compiler));
	hash = crispy_cache_provider_compute_hash(
		CRISPY_CACHE_PROVIDER(self->cache),
		source_content, -1,
		extra_flags,
		compiler_version);

	/* get the cache path for this hash */
	so_path = crispy_cache_provider_get_path(
		CRISPY_CACHE_PROVIDER(self->cache), hash);

	/* check cache unless force recompilation */
	if (!force &&
	    crispy_cache_provider_has_valid(
	        CRISPY_CACHE_PROVIDER(self->cache),
	        hash, source_path)) {
		g_debug("Bar C config cache hit: %s", so_path);
		return so_path;
	}

	/* compile the source to a shared object */
	g_debug("Bar C config compile: %s -> %s", source_path, so_path);
	if (!crispy_compiler_compile_shared(
	        CRISPY_COMPILER(self->compiler),
	        source_path,
	        so_path,
	        extra_flags,
	        error)) {
		g_free(so_path);
		return NULL;
	}

	return so_path;
}

/**
 * gowlbar_config_compiler_load_and_apply:
 * @self: a #GowlbarConfigCompiler
 * @so_path: path to the compiled shared object
 * @error: (nullable): return location for a #GError, or %NULL
 *
 * Opens the shared object at @so_path, looks up the
 * `gowlbar_config_init` symbol, and calls it.
 *
 * Returns: %TRUE if the config was loaded and applied successfully,
 *          %FALSE on error
 */
gboolean
gowlbar_config_compiler_load_and_apply(
	GowlbarConfigCompiler  *self,
	const gchar            *so_path,
	GError                **error
){
	GModule *module;
	gpointer symbol;
	gboolean (*config_init_fn)(void);
	gboolean result;

	g_return_val_if_fail(GOWLBAR_IS_CONFIG_COMPILER(self), FALSE);
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

	if (!g_module_symbol(module, "gowlbar_config_init", &symbol)) {
		g_set_error(error,
		            G_IO_ERROR,
		            G_IO_ERROR_NOT_FOUND,
		            "Symbol 'gowlbar_config_init' not found in '%s': %s",
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
		            "gowlbar_config_init() returned FALSE in '%s'",
		            so_path);
		g_module_close(module);
		return FALSE;
	}

	/* keep the module open so symbols remain available */

	return TRUE;
}
