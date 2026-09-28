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

/*
 * gowl-macro-loader.c - see the header.
 */

#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "gowl-macro"

#include "gowl-macro-loader.h"
#include "util/gowl-fault-guard.h"

#include <crispy.h>
#include <string.h>

/* The system directories, spelled out: the build's GOWL_DATADIR and
   GOWL_SYSCONFDIR lose their quotes on the way into a module's
   compiler flags (the bar module has the same note). */
static const gchar *const system_dirs[] = {
	"/etc/gowl/macros",
	"/usr/local/share/gowl/macros",
	"/usr/share/gowl/macros",
	NULL
};

/* Keeps open-time code (constructors) from running forever. */
#define LOAD_TIMEOUT_MS (5000)

struct _GowlMacroLoader {
	GStrv              dirs;
	gchar             *cache_dir;
	CrispyGccCompiler *compiler;
	CrispyFileCache   *cache;
	gboolean           compiler_failed;
	/* resolved path -> GowlMacroScript* (not owned: see retired) */
	GHashTable        *scripts;
	/* Every script ever opened, freed only with the loader.  A threaded
	   run holds a GowlMacroScript* until its worker reads it, and a
	   recompile or a reload in between would otherwise free it under
	   the worker.  They are a few dozen bytes each. */
	GPtrArray         *retired;
};

G_DEFINE_QUARK(gowl-macro-loader-error-quark, gowl_macro_loader_error)

static void
script_free(gpointer data)
{
	GowlMacroScript *s = data;

	/* The GModule stays open for good (made resident): a run in flight
	   on a worker, or a step queued on the timeline, may still point
	   into it. */
	g_free(s->name);
	g_free(s->path);
	g_free(s->so_path);
	g_free(s->hash);
	g_free(s->description);
	g_free(s);
}

/**
 * gowl_macro_loader_new: (skip)
 * @cache_dir: (nullable): where compiled macros go; default
 *   $XDG_CACHE_HOME/gowl/macros
 *
 * Returns: (transfer full): a loader searching the default directories
 */
GowlMacroLoader *
gowl_macro_loader_new(const gchar *cache_dir)
{
	GowlMacroLoader *self;

	self = g_new0(GowlMacroLoader, 1);
	self->cache_dir = cache_dir != NULL
		? g_strdup(cache_dir)
		: g_build_filename(g_get_user_cache_dir(), "gowl", "macros", NULL);
	self->dirs = gowl_macro_loader_default_dirs(NULL);
	self->scripts = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
	                                      NULL);
	self->retired = g_ptr_array_new_with_free_func(script_free);
	return self;
}

/**
 * gowl_macro_loader_free: (skip)
 * @self: (nullable): a loader
 */
void
gowl_macro_loader_free(GowlMacroLoader *self)
{
	if (self == NULL)
		return;
	g_strfreev(self->dirs);
	g_free(self->cache_dir);
	g_clear_object(&self->compiler);
	g_clear_object(&self->cache);
	g_hash_table_unref(self->scripts);
	g_ptr_array_unref(self->retired);
	g_free(self);
}

/**
 * gowl_macro_loader_default_dirs: (skip)
 * @configured: (nullable): the `macro-dir' setting, `:'-separated
 *
 * The search path, in order: @configured, $GOWL_MACRO_DIR,
 * $XDG_CONFIG_HOME/gowl/macros, $XDG_DATA_HOME/gowl/macros,
 * /etc/gowl/macros, /usr/local/share/gowl/macros, /usr/share/gowl/macros,
 * and -- in a development build -- the shipped examples in the source
 * tree.  Duplicates are dropped.  `~/' is expanded.
 *
 * Returns: (transfer full): the directories
 */
GStrv
gowl_macro_loader_default_dirs(const gchar *configured)
{
	g_autoptr(GPtrArray) out = NULL;
	const gchar *env;
	guint i;

	out = g_ptr_array_new_with_free_func(g_free);

#define ADD_DIR(d) \
	do { \
		gchar *dir_ = (d); \
		gboolean dup_ = FALSE; \
		guint j_; \
		for (j_ = 0; j_ < out->len; j_++) \
			if (g_strcmp0(g_ptr_array_index(out, j_), dir_) == 0) \
				dup_ = TRUE; \
		if (dup_ || dir_ == NULL || *dir_ == '\0') \
			g_free(dir_); \
		else \
			g_ptr_array_add(out, dir_); \
	} while (0)

	if (configured != NULL) {
		g_auto(GStrv) parts = g_strsplit(configured, ":", -1);

		for (i = 0; parts[i] != NULL; i++) {
			g_strstrip(parts[i]);
			if (g_str_has_prefix(parts[i], "~/"))
				ADD_DIR(g_build_filename(g_get_home_dir(), parts[i] + 2,
				                         NULL));
			else
				ADD_DIR(g_strdup(parts[i]));
		}
	}
	env = g_getenv("GOWL_MACRO_DIR");
	if (env != NULL) {
		g_auto(GStrv) parts = g_strsplit(env, ":", -1);

		for (i = 0; parts[i] != NULL; i++)
			ADD_DIR(g_strdup(parts[i]));
	}
	ADD_DIR(g_build_filename(g_get_user_config_dir(), "gowl", "macros", NULL));
	ADD_DIR(g_build_filename(g_get_user_data_dir(), "gowl", "macros", NULL));
	for (i = 0; system_dirs[i] != NULL; i++)
		ADD_DIR(g_strdup(system_dirs[i]));
#ifdef GOWL_MACRO_DEV_MACROS
	/* The shipped examples, from a build tree, after everything else */
	if (g_file_test(GOWL_MACRO_DEV_MACROS, G_FILE_TEST_IS_DIR))
		ADD_DIR(g_strdup(GOWL_MACRO_DEV_MACROS));
#endif

#undef ADD_DIR

	g_ptr_array_add(out, NULL);
	return (GStrv)g_ptr_array_free(g_steal_pointer(&out), FALSE);
}

/**
 * gowl_macro_loader_set_dirs: (skip)
 * @self: a loader
 * @dirs: the search path, in order
 */
void
gowl_macro_loader_set_dirs(
	GowlMacroLoader     *self,
	const gchar * const *dirs
){
	g_return_if_fail(self != NULL);
	g_strfreev(self->dirs);
	self->dirs = g_strdupv((gchar **)dirs);
}

/**
 * gowl_macro_loader_get_dirs: (skip)
 * @self: a loader
 *
 * Returns: (transfer none): the search path
 */
const gchar * const *
gowl_macro_loader_get_dirs(GowlMacroLoader *self)
{
	g_return_val_if_fail(self != NULL, NULL);
	return (const gchar * const *)self->dirs;
}

/**
 * gowl_macro_loader_normalize_name: (skip)
 * @spec: a name or path
 *
 * Returns: (transfer full): the display name: basename without a `.c'
 *   or `.so' suffix
 */
gchar *
gowl_macro_loader_normalize_name(const gchar *spec)
{
	gchar *base;

	g_return_val_if_fail(spec != NULL, NULL);
	base = g_path_get_basename(spec);
	if (g_str_has_suffix(base, ".c"))
		base[strlen(base) - 2] = '\0';
	else if (g_str_has_suffix(base, ".so"))
		base[strlen(base) - 3] = '\0';
	return base;
}

static gboolean
is_regular(const gchar *path)
{
	return g_file_test(path, G_FILE_TEST_IS_REGULAR);
}

/**
 * gowl_macro_loader_resolve: (skip)
 * @self: a loader
 * @spec: `name', `name.c', `name.so', or a path containing `/'
 * @error: return location for a #GError
 *
 * Returns: (transfer full) (nullable): the file to load
 */
gchar *
gowl_macro_loader_resolve(
	GowlMacroLoader  *self,
	const gchar      *spec,
	GError          **error
){
	static const gchar *const suffixes[] = { "", ".c", ".so", NULL };
	guint i;
	guint j;

	g_return_val_if_fail(self != NULL, NULL);
	g_return_val_if_fail(spec != NULL, NULL);

	/* A path: no lookup at all */
	if (strchr(spec, '/') != NULL) {
		g_autofree gchar *expanded = NULL;

		expanded = g_str_has_prefix(spec, "~/")
			? g_build_filename(g_get_home_dir(), spec + 2, NULL)
			: g_strdup(spec);
		if (is_regular(expanded))
			return g_steal_pointer(&expanded);
		g_set_error(error, GOWL_MACRO_LOADER_ERROR,
		            GOWL_MACRO_LOADER_ERROR_NOT_FOUND,
		            "no macro file %s", expanded);
		return NULL;
	}
	if (*spec == '\0' || strcmp(spec, ".") == 0 || strcmp(spec, "..") == 0) {
		g_set_error(error, GOWL_MACRO_LOADER_ERROR,
		            GOWL_MACRO_LOADER_ERROR_NOT_FOUND,
		            "`%s' is not a macro name", spec);
		return NULL;
	}

	for (i = 0; self->dirs != NULL && self->dirs[i] != NULL; i++) {
		for (j = 0; suffixes[j] != NULL; j++) {
			g_autofree gchar *file = g_strconcat(spec, suffixes[j], NULL);
			g_autofree gchar *path = g_build_filename(self->dirs[i], file,
			                                          NULL);

			/* `name' alone must not pick up a directory, and a bare
			   file with no suffix is taken only if it ends in .c/.so */
			if (j == 0 && !g_str_has_suffix(spec, ".c")
			    && !g_str_has_suffix(spec, ".so"))
				continue;
			if (is_regular(path))
				return g_steal_pointer(&path);
		}
	}
	g_set_error(error, GOWL_MACRO_LOADER_ERROR,
	            GOWL_MACRO_LOADER_ERROR_NOT_FOUND,
	            "no macro named %s in the macro path", spec);
	return NULL;
}

/**
 * gowl_macro_loader_discover: (skip)
 * @self: a loader
 *
 * Every macro file on the search path, the first of each name only
 * (the one a bare name would run), sorted by name.
 *
 * Returns: (transfer full) (element-type utf8): pairs flattened as
 *   name, path, name, path, ...
 */
GPtrArray *
gowl_macro_loader_discover(GowlMacroLoader *self)
{
	g_autoptr(GHashTable) seen = NULL;
	g_autoptr(GPtrArray) names = NULL;
	GPtrArray *out;
	guint i;

	g_return_val_if_fail(self != NULL, NULL);

	seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	for (i = 0; self->dirs != NULL && self->dirs[i] != NULL; i++) {
		g_autoptr(GDir) dir = g_dir_open(self->dirs[i], 0, NULL);
		const gchar *entry;

		if (dir == NULL)
			continue;
		while ((entry = g_dir_read_name(dir)) != NULL) {
			g_autofree gchar *path = NULL;
			gchar *name;

			if (!g_str_has_suffix(entry, ".c")
			    && !g_str_has_suffix(entry, ".so"))
				continue;
			path = g_build_filename(self->dirs[i], entry, NULL);
			if (!is_regular(path))
				continue;
			name = gowl_macro_loader_normalize_name(entry);
			if (g_hash_table_contains(seen, name)) {
				g_free(name);
				continue;
			}
			g_hash_table_insert(seen, name, g_steal_pointer(&path));
		}
	}

	names = g_hash_table_get_keys_as_ptr_array(seen);
	g_ptr_array_sort_values(names, (GCompareFunc)g_strcmp0);
	out = g_ptr_array_new_with_free_func(g_free);
	for (i = 0; i < names->len; i++) {
		const gchar *n = g_ptr_array_index(names, i);

		g_ptr_array_add(out, g_strdup(n));
		g_ptr_array_add(out, g_strdup(g_hash_table_lookup(seen, n)));
	}
	return out;
}

/* --- compiling --- */

static gboolean
ensure_compiler(
	GowlMacroLoader  *self,
	GError          **error
){
	if (self->compiler != NULL)
		return TRUE;
	if (self->compiler_failed) {
		g_set_error(error, GOWL_MACRO_LOADER_ERROR,
		            GOWL_MACRO_LOADER_ERROR_COMPILE,
		            "no C compiler (gcc) is available to build macros");
		return FALSE;
	}
	self->compiler = crispy_gcc_compiler_new(error);
	if (self->compiler == NULL) {
		self->compiler_failed = TRUE;
		return FALSE;
	}
	self->cache = crispy_file_cache_new_with_dir(self->cache_dir);
	return TRUE;
}

/* The flags a macro compiles with: the in-tree headers when this is a
   development build (so a machine with gowl also installed never mixes
   the two), else pkg-config's; plus what gowl.h pulls in. */
static gchar *
include_flags(void)
{
	g_autofree gchar *deps = NULL;
	gint status;

	if (!g_spawn_command_line_sync(
		    "pkg-config --cflags cairo pangocairo wayland-server "
		    "json-glib-1.0", &deps, NULL, &status, NULL)
	    || !g_spawn_check_wait_status(status, NULL)) {
		g_clear_pointer(&deps, g_free);
		deps = g_strdup("");
	}
	g_strstrip(deps);

#ifdef GOWL_MACRO_DEV_INCLUDE
	if (g_file_test(GOWL_MACRO_DEV_INCLUDE "/gowl/gowl.h",
	                G_FILE_TEST_EXISTS))
		return g_strdup_printf("-I%s -I%s/gowl -DWLR_USE_UNSTABLE %s",
		                       GOWL_MACRO_DEV_INCLUDE,
		                       GOWL_MACRO_DEV_INCLUDE, deps);
#endif
	{
		g_autofree gchar *pkg = NULL;

		if (g_spawn_command_line_sync("pkg-config --cflags gowl", &pkg,
		                              NULL, &status, NULL)
		    && g_spawn_check_wait_status(status, NULL) && pkg != NULL) {
			g_strstrip(pkg);
			return g_strdup_printf("%s -DWLR_USE_UNSTABLE %s", pkg, deps);
		}
	}
	return g_strdup_printf("-DWLR_USE_UNSTABLE %s", deps);
}

/* The value of `#define NAME ...' in @source, or NULL. */
static gchar *
source_define(
	const gchar *source,
	const gchar *name
){
	g_autofree gchar *pattern = NULL;
	g_autoptr(GRegex) re = NULL;
	g_autoptr(GMatchInfo) m = NULL;

	pattern = g_strdup_printf("^[ \\t]*#[ \\t]*define[ \\t]+%s[ \\t]+(.+?)"
	                          "[ \\t]*$", name);
	re = g_regex_new(pattern, G_REGEX_MULTILINE, 0, NULL);
	if (re == NULL || !g_regex_match(re, source, 0, &m))
		return NULL;
	return g_match_info_fetch(m, 1);
}

/* CRISPY_PARAMS, shell-expanded as a C config's are: `$(pkg-config ...)'
   works.  The macro is the user's own code on the user's own desktop;
   its build flags are trusted the same way. */
static gchar *
crispy_params(
	const gchar  *source,
	GError      **error
){
	g_autofree gchar *raw = NULL;
	g_autofree gchar *cmd = NULL;
	g_autofree gchar *out = NULL;
	g_autofree gchar *err = NULL;
	gint status;

	raw = source_define(source, "CRISPY_PARAMS");
	if (raw == NULL)
		return g_strdup("");
	cmd = g_strdup_printf("/bin/sh -c \"printf '%%s' %s\"", raw);
	if (!g_spawn_command_line_sync(cmd, &out, &err, &status, error))
		return NULL;
	if (!g_spawn_check_wait_status(status, NULL)) {
		g_set_error(error, GOWL_MACRO_LOADER_ERROR,
		            GOWL_MACRO_LOADER_ERROR_COMPILE,
		            "CRISPY_PARAMS did not expand: %s",
		            err != NULL ? err : "");
		return NULL;
	}
	g_strstrip(out);
	return g_steal_pointer(&out);
}

/* Compiles @path (a .c) if its cached build is stale; sets *out_hash. */
static gchar *
compile_source(
	GowlMacroLoader  *self,
	const gchar      *path,
	const gchar      *source,
	gchar           **out_hash,
	GError          **error
){
	g_autofree gchar *params = NULL;
	g_autofree gchar *includes = NULL;
	g_autofree gchar *flags = NULL;
	gchar *hash;
	gchar *so_path;

	if (!ensure_compiler(self, error))
		return NULL;
	params = crispy_params(source, error);
	if (params == NULL)
		return NULL;
	includes = include_flags();
	flags = g_strdup_printf("%s %s", includes, params);

	hash = crispy_cache_provider_compute_hash(
		CRISPY_CACHE_PROVIDER(self->cache), source, -1, flags,
		crispy_compiler_get_version(CRISPY_COMPILER(self->compiler)));
	so_path = crispy_cache_provider_get_path(
		CRISPY_CACHE_PROVIDER(self->cache), hash);
	*out_hash = hash;

	if (crispy_cache_provider_has_valid(CRISPY_CACHE_PROVIDER(self->cache),
	                                    hash, path))
		return so_path;

	g_message("macro: compiling %s", path);
	if (!crispy_compiler_compile_shared(CRISPY_COMPILER(self->compiler),
	                                    path, so_path, flags, error)) {
		g_free(so_path);
		if (error != NULL && *error != NULL) {
			/* A compile error spans lines; replies are one line. */
			GError *e = *error;
			gchar *flat = g_strdelimit(g_strdup(e->message), "\n", ' ');

			*error = g_error_new(GOWL_MACRO_LOADER_ERROR,
			                     GOWL_MACRO_LOADER_ERROR_COMPILE,
			                     "%s did not compile: %s", path, flat);
			g_free(flat);
			g_error_free(e);
		}
		return NULL;
	}
	return so_path;
}

/* --- opening --- */

typedef struct {
	const gchar *so_path;
	GModule     *module;
} OpenCall;

static void
open_body(gpointer data)
{
	OpenCall *o = data;

	o->module = g_module_open(o->so_path, G_MODULE_BIND_LAZY);
	if (o->module != NULL)
		g_module_make_resident(o->module);
}

/* gowl_macro_info() is macro code like any other: called guarded. */
typedef struct {
	const gchar *(*info)(void);
	gchar        *text;
} InfoCall;

static void
info_body(gpointer data)
{
	InfoCall *c = data;
	const gchar *t;

	t = c->info();
	c->text = g_strdup(t);
}

static gboolean
bool_define(
	const gchar *source,
	const gchar *name
){
	g_autofree gchar *v = source_define(source, name);

	return v != NULL && (g_strcmp0(v, "1") == 0 || g_ascii_strcasecmp(v, "TRUE") == 0);
}

/*
 * Opens @so_path under the guard and reads what it exports.  The
 * run function is required; the ABI symbol, when present, must match.
 */
static GowlMacroScript *
open_script(
	const gchar  *name,
	const gchar  *path,
	const gchar  *so_path,
	const gchar  *source,
	GError      **error
){
	OpenCall o;
	gint signo;
	gpointer sym;
	GowlMacroScript *s;

	o.so_path = so_path;
	o.module = NULL;
	if (!gowl_fault_guard_call(open_body, &o, LOAD_TIMEOUT_MS, &signo,
	                           "opening a macro")) {
		g_set_error(error, GOWL_MACRO_LOADER_ERROR,
		            GOWL_MACRO_LOADER_ERROR_FAULT,
		            "%s crashed while loading (%s)", path,
		            gowl_fault_guard_signal_name(signo));
		return NULL;
	}
	if (o.module == NULL) {
		g_set_error(error, GOWL_MACRO_LOADER_ERROR,
		            GOWL_MACRO_LOADER_ERROR_LOAD,
		            "%s did not load: %s", path, g_module_error());
		return NULL;
	}
	if (g_module_symbol(o.module, "gowl_macro_abi", &sym) && sym != NULL
	    && *(const guint *)sym != GOWL_MACRO_ABI) {
		g_set_error(error, GOWL_MACRO_LOADER_ERROR,
		            GOWL_MACRO_LOADER_ERROR_ABI,
		            "%s was written for macro ABI %u; this gowl has %u",
		            path, *(const guint *)sym, (guint)GOWL_MACRO_ABI);
		return NULL;
	}
	if (!g_module_symbol(o.module, "gowl_macro_run", &sym) || sym == NULL) {
		g_set_error(error, GOWL_MACRO_LOADER_ERROR,
		            GOWL_MACRO_LOADER_ERROR_LOAD,
		            "%s exports no gowl_macro_run() (declare it "
		            "G_MODULE_EXPORT)", path);
		return NULL;
	}

	s = g_new0(GowlMacroScript, 1);
	s->name = g_strdup(name);
	s->path = g_strdup(path);
	s->so_path = g_strdup(so_path);
	s->run = (GowlMacroScriptRun)sym;

	if (g_module_symbol(o.module, "gowl_macro_info", &sym) && sym != NULL) {
		InfoCall ic;

		ic.info = (const gchar *(*)(void))sym;
		ic.text = NULL;
		if (!gowl_fault_guard_call(info_body, &ic, LOAD_TIMEOUT_MS, &signo,
		                           "a macro's gowl_macro_info()")) {
			g_set_error(error, GOWL_MACRO_LOADER_ERROR,
			            GOWL_MACRO_LOADER_ERROR_FAULT,
			            "%s crashed in gowl_macro_info() (%s)", path,
			            gowl_fault_guard_signal_name(signo));
			script_free(s);
			return NULL;
		}
		s->description = ic.text;
	}

	/* Mode and budget: from the source's #defines, or -- for a .so
	   given directly -- from exported constants. */
	if (source != NULL) {
		g_autofree gchar *t = source_define(source, "GOWL_MACRO_TIMEOUT_MS");

		s->threaded = bool_define(source, "GOWL_MACRO_THREADED");
		if (t != NULL) {
			s->timeout_ms = (guint)g_ascii_strtoull(t, NULL, 10);
			s->has_timeout = TRUE;
		}
	} else {
		if (g_module_symbol(o.module, "gowl_macro_threaded", &sym)
		    && sym != NULL)
			s->threaded = *(const gboolean *)sym;
		if (g_module_symbol(o.module, "gowl_macro_timeout_ms", &sym)
		    && sym != NULL) {
			s->timeout_ms = *(const guint *)sym;
			s->has_timeout = TRUE;
		}
	}
	return s;
}

/**
 * gowl_macro_loader_load: (skip)
 * @self: a loader
 * @spec: a name or path
 * @error: return location for a #GError
 *
 * Resolves, compiles when the source changed, and opens.  The script
 * stays cached until the source changes or gowl_macro_loader_forget().
 *
 * Returns: (transfer none) (nullable): the loaded macro
 */
const GowlMacroScript *
gowl_macro_loader_load(
	GowlMacroLoader  *self,
	const gchar      *spec,
	GError          **error
){
	g_autofree gchar *path = NULL;
	g_autofree gchar *name = NULL;
	g_autofree gchar *source = NULL;
	g_autofree gchar *so_path = NULL;
	g_autofree gchar *hash = NULL;
	GowlMacroScript *cached;
	GowlMacroScript *s;

	g_return_val_if_fail(self != NULL, NULL);
	g_return_val_if_fail(spec != NULL, NULL);

	path = gowl_macro_loader_resolve(self, spec, error);
	if (path == NULL)
		return NULL;
	name = gowl_macro_loader_normalize_name(path);
	cached = g_hash_table_lookup(self->scripts, path);

	if (g_str_has_suffix(path, ".so")) {
		if (cached != NULL)
			return cached;
		s = open_script(name, path, path, NULL, error);
	} else {
		if (!g_file_get_contents(path, &source, NULL, error))
			return NULL;
		so_path = compile_source(self, path, source, &hash, error);
		if (so_path == NULL)
			return NULL;
		/* Unchanged source: the macro already open is the one */
		if (cached != NULL && g_strcmp0(cached->hash, hash) == 0)
			return cached;
		s = open_script(name, path, so_path, source, error);
		if (s != NULL)
			s->hash = g_steal_pointer(&hash);
	}
	if (s == NULL)
		return NULL;
	g_ptr_array_add(self->retired, s);
	g_hash_table_replace(self->scripts, g_strdup(path), s);
	return s;
}

/**
 * gowl_macro_loader_compile_only: (skip)
 * @self: a loader
 * @spec: a name or path
 * @error: return location for a #GError
 *
 * Checks that a macro compiles, without opening it.
 *
 * Returns: %TRUE when it compiles (or is a .so)
 */
gboolean
gowl_macro_loader_compile_only(
	GowlMacroLoader  *self,
	const gchar      *spec,
	GError          **error
){
	g_autofree gchar *path = NULL;
	g_autofree gchar *source = NULL;
	g_autofree gchar *so_path = NULL;
	g_autofree gchar *hash = NULL;

	g_return_val_if_fail(self != NULL, FALSE);
	path = gowl_macro_loader_resolve(self, spec, error);
	if (path == NULL)
		return FALSE;
	if (g_str_has_suffix(path, ".so"))
		return TRUE;
	if (!g_file_get_contents(path, &source, NULL, error))
		return FALSE;
	so_path = compile_source(self, path, source, &hash, error);
	return so_path != NULL;
}

/**
 * gowl_macro_loader_forget: (skip)
 * @self: a loader
 *
 * Drops the cache of opened macros, so each is looked up (and its
 * source re-checked) again.  Opened code stays mapped, and the scripts
 * themselves stay allocated until the loader goes: a threaded run may
 * still be about to read one.
 */
void
gowl_macro_loader_forget(GowlMacroLoader *self)
{
	g_return_if_fail(self != NULL);
	g_hash_table_remove_all(self->scripts);
}
