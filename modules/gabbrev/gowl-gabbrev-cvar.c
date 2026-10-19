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
 * gowl-gabbrev-cvar.c - Compiling, opening and calling `c' variables.
 *
 * The compiler and its flags are the macro module's (see
 * modules/macro/gowl-macro-loader.c): the in-tree headers in a
 * development build, pkg-config's otherwise, and CRISPY_PARAMS from the
 * source.  A variable is the user's own code on the user's own desktop,
 * so its flags are trusted the same way.
 */

#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "gowl-gabbrev"

#include "gowl-gabbrev-cvar.h"
#include "gabbrev/gowl-gabbrev-private.h"
#include "util/gowl-fault-guard.h"

#include <crispy.h>
#include <gio/gio.h>
#include <gmodule.h>
#include <string.h>

/* Opening a build runs its constructors: a short budget */
#define OPEN_TIMEOUT_MS (2000)

/**
 * gabbrev_cvar_spec_clear: (skip)
 * @spec: a spec
 */
void
gabbrev_cvar_spec_clear(GabbrevCvarSpec *spec)
{
	g_clear_pointer(&spec->code, g_free);
	g_clear_pointer(&spec->source, g_free);
	g_clear_pointer(&spec->file, g_free);
	g_clear_pointer(&spec->cflags, g_free);
}

/**
 * gabbrev_cvar_spec_from_params: (skip)
 * @spec: a zeroed spec to fill
 * @params: a `c' variable's params
 * @error: why they do not describe a build
 *
 * Returns: %TRUE when exactly one of code, source and file was given
 */
gboolean
gabbrev_cvar_spec_from_params(
	GabbrevCvarSpec  *spec,
	GHashTable       *params,
	GError          **error
){
	const gchar *code = g_hash_table_lookup(params, "code");
	const gchar *source = g_hash_table_lookup(params, "source");
	const gchar *file = g_hash_table_lookup(params, "file");
	guint n = (code != NULL) + (source != NULL) + (file != NULL);

	if (n != 1) {
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
		            "c: give exactly one of params.func, code, source "
		            "and file");
		return FALSE;
	}
	spec->code = g_strdup(code);
	spec->source = g_strdup(source);
	if (file != NULL) {
		if (g_str_has_prefix(file, "~/"))
			spec->file = g_build_filename(g_get_home_dir(), file + 2, NULL);
		else
			spec->file = g_strdup(file);
	}
	spec->cflags = g_strdup(g_hash_table_lookup(params, "cflags"));
	return TRUE;
}

/**
 * gabbrev_cvar_full_source: (skip)
 * @spec: a spec
 * @error: why the source cannot be had
 *
 * Returns: (transfer full) (nullable): the translation unit to build
 */
gchar *
gabbrev_cvar_full_source(
	const GabbrevCvarSpec  *spec,
	GError                **error
){
	gchar *text = NULL;

	if (spec->file != NULL) {
		if (!g_file_get_contents(spec->file, &text, NULL, error))
			return NULL;
		return text;
	}
	if (spec->source != NULL)
		return g_strdup(spec->source);
	/* A body: the function around it, with two shorthands.  #line makes
	   a compile error point into the snippet rather than the wrapper. */
	return g_strdup_printf(
		"#include <gowl/gowl.h>\n"
		"#define PARAM(k) gowl_gabbrev_request_get_param(req, (k))\n"
		"#define VAR(n) gowl_gabbrev_request_get_var(req, (n))\n"
		"G_MODULE_EXPORT gchar *" GABBREV_CVAR_SYMBOL
		"(GowlGabbrevRequest *req);\n"
		"G_MODULE_EXPORT gchar *\n" GABBREV_CVAR_SYMBOL
		"(GowlGabbrevRequest *req)\n{\n"
		"#line 1 \"gabbrev-c-variable\"\n"
		"%s\n}\n", spec->code != NULL ? spec->code : "return NULL;");
}

/**
 * gabbrev_cvar_spec_key: (skip)
 * @spec: a spec
 *
 * Returns: (transfer full): what identifies the build: its text and flags
 *   for code and source, its path for a file (whose content is checked
 *   by the cache when it is built)
 */
gchar *
gabbrev_cvar_spec_key(const GabbrevCvarSpec *spec)
{
	return g_compute_checksum_for_string(G_CHECKSUM_SHA256,
		spec->file != NULL ? spec->file
		: spec->source != NULL ? spec->source : spec->code, -1);
}

/* --- building --- */

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
#ifdef GABBREV_DEV_INCLUDE
	if (g_file_test(GABBREV_DEV_INCLUDE "/gowl/gowl.h", G_FILE_TEST_EXISTS))
		return g_strdup_printf("-I%s -I%s/gowl -DWLR_USE_UNSTABLE %s",
		                       GABBREV_DEV_INCLUDE, GABBREV_DEV_INCLUDE, deps);
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

/* `$(pkg-config ...)' in extra flags, as in CRISPY_PARAMS */
static gchar *
expand_flags(
	const gchar  *raw,
	GError      **error
){
	g_autofree gchar *inner = NULL;
	g_autofree gchar *quoted = NULL;
	g_autofree gchar *cmd = NULL;
	gchar *out = NULL;
	gint status;

	if (raw == NULL || *raw == '\0')
		return g_strdup("");
	inner = g_strdup_printf("printf '%%s' %s", raw);
	quoted = g_shell_quote(inner);
	cmd = g_strdup_printf("/bin/sh -c %s", quoted);
	if (!g_spawn_command_line_sync(cmd, &out, NULL, &status, error))
		return NULL;
	if (!g_spawn_check_wait_status(status, error)) {
		g_free(out);
		return NULL;
	}
	return g_strstrip(out);
}

/* The `#define CRISPY_PARAMS ...' of a source, or NULL. */
static gchar *
crispy_params_define(const gchar *source)
{
	g_autoptr(GRegex) re = NULL;
	g_autoptr(GMatchInfo) m = NULL;

	re = g_regex_new("^[ \\t]*#[ \\t]*define[ \\t]+CRISPY_PARAMS[ \\t]+"
	                 "\"?(.+?)\"?[ \\t]*$", G_REGEX_MULTILINE, 0, NULL);
	if (re == NULL || !g_regex_match(re, source, 0, &m))
		return NULL;
	return g_match_info_fetch(m, 1);
}

/**
 * gabbrev_cvar_build: (skip)
 * @spec: what to build
 * @cache_dir: where builds (and written-out snippets) are kept
 * @error: a one-line compile error
 *
 * Any thread.  An unchanged source is not rebuilt.
 *
 * Returns: (transfer full) (nullable): the shared object's path
 */
gchar *
gabbrev_cvar_build(
	const GabbrevCvarSpec  *spec,
	const gchar            *cache_dir,
	GError                **error
){
	g_autoptr(CrispyGccCompiler) compiler = NULL;
	g_autoptr(CrispyFileCache) cache = NULL;
	g_autofree gchar *source = NULL;
	g_autofree gchar *includes = NULL;
	g_autofree gchar *extra = NULL;
	g_autofree gchar *from_source = NULL;
	g_autofree gchar *flags = NULL;
	g_autofree gchar *hash = NULL;
	g_autofree gchar *src_path = NULL;
	gchar *so_path;

	source = gabbrev_cvar_full_source(spec, error);
	if (source == NULL)
		return NULL;
	compiler = crispy_gcc_compiler_new(error);
	if (compiler == NULL)
		return NULL;
	if (g_mkdir_with_parents(cache_dir, 0700) != 0) {
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
		            "cannot create %s", cache_dir);
		return NULL;
	}
	cache = crispy_file_cache_new_with_dir(cache_dir);

	includes = include_flags();
	extra = expand_flags(spec->cflags, error);
	if (extra == NULL)
		return NULL;
	{
		g_autofree gchar *raw = crispy_params_define(source);

		from_source = expand_flags(raw, error);
		if (from_source == NULL)
			return NULL;
	}
	flags = g_strdup_printf("%s %s %s", includes, extra, from_source);

	hash = crispy_cache_provider_compute_hash(CRISPY_CACHE_PROVIDER(cache),
		source, -1, flags,
		crispy_compiler_get_version(CRISPY_COMPILER(compiler)));
	so_path = crispy_cache_provider_get_path(CRISPY_CACHE_PROVIDER(cache),
	                                         hash);

	/* A file builds from where it is; text is written out first */
	if (spec->file != NULL) {
		src_path = g_strdup(spec->file);
	} else {
		g_autofree gchar *name = g_strdup_printf("gabbrev-%s.c", hash);

		src_path = g_build_filename(cache_dir, name, NULL);
		if (!g_file_test(src_path, G_FILE_TEST_EXISTS)
		    && !g_file_set_contents(src_path, source, -1, error)) {
			g_free(so_path);
			return NULL;
		}
	}
	if (crispy_cache_provider_has_valid(CRISPY_CACHE_PROVIDER(cache), hash,
	                                    src_path))
		return so_path;

	if (!crispy_compiler_compile_shared(CRISPY_COMPILER(compiler), src_path,
	                                    so_path, flags, error)) {
		g_free(so_path);
		if (error != NULL && *error != NULL) {
			GError *e = *error;
			g_autofree gchar *flat = g_strdelimit(g_strdup(e->message),
			                                      "\n", ' ');

			*error = g_error_new(G_IO_ERROR, G_IO_ERROR_FAILED,
			                     "c variable did not compile: %s", flat);
			g_error_free(e);
		}
		return NULL;
	}
	return so_path;
}

/* --- opening and calling, under the guard --- */

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

/**
 * gabbrev_cvar_open: (skip)
 * @so_path: a build
 * @error: why it did not open
 *
 * Compositor thread.  The build stays loaded for the life of the
 * process (a GModule that code may still be running in is never
 * closed; a rebuilt variable is a new file).
 *
 * Returns: (nullable): its function
 */
GabbrevCvarFunc
gabbrev_cvar_open(
	const gchar  *so_path,
	GError      **error
){
	OpenCall o;
	gint signo = 0;
	gpointer sym = NULL;

	o.so_path = so_path;
	o.module = NULL;
	if (!gowl_fault_guard_call(open_body, &o, OPEN_TIMEOUT_MS, &signo,
	                           "opening a gabbrev c variable")) {
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
		            "crashed while loading (%s)",
		            gowl_fault_guard_signal_name(signo));
		return NULL;
	}
	if (o.module == NULL) {
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "%s",
		            g_module_error());
		return NULL;
	}
	if (!g_module_symbol(o.module, GABBREV_CVAR_SYMBOL, &sym) || sym == NULL) {
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
		            "it does not export " GABBREV_CVAR_SYMBOL "()");
		return NULL;
	}
	return (GabbrevCvarFunc)sym;
}

typedef struct {
	GabbrevCvarFunc     func;
	GowlGabbrevHandle  *handle;
	GowlGabbrevRequest *request;
	gchar              *result;
} CallBody;

static void
call_body(gpointer data)
{
	CallBody *c = data;

	if (c->func != NULL)
		c->result = c->func(c->request);
	else
		c->result = gowl_gabbrev_handle_call_func(c->handle, c->request);
}

static gchar *
guarded(
	CallBody  *c,
	guint      timeout_ms,
	GError   **error
){
	gint signo = 0;

	if (!gowl_fault_guard_call(call_body, c, timeout_ms, &signo,
	                           "a gabbrev c variable")) {
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
		            "%s and was stopped",
		            gowl_fault_guard_signal_name(signo));
		return NULL;
	}
	if (c->result == NULL)
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
		            "the function returned NULL");
	return c->result;
}

/**
 * gabbrev_cvar_call: (skip)
 * @func: an opened variable
 * @request: its request
 * @timeout_ms: its budget
 * @error: a fault or a NULL return
 *
 * Returns: (transfer full) (nullable): the value
 */
gchar *
gabbrev_cvar_call(
	GabbrevCvarFunc      func,
	GowlGabbrevRequest  *request,
	guint                timeout_ms,
	GError             **error
){
	CallBody c;

	memset(&c, 0, sizeof c);
	c.func = func;
	c.request = request;
	return guarded(&c, timeout_ms, error);
}

/**
 * gabbrev_cvar_call_registered: (skip)
 * @name: a name given to gowl_gabbrev_register_func()
 * @request: its request
 * @timeout_ms: its budget
 * @error: unknown name, a fault, or a NULL return
 *
 * Returns: (transfer full) (nullable): the value
 */
gchar *
gabbrev_cvar_call_registered(
	const gchar         *name,
	GowlGabbrevRequest  *request,
	guint                timeout_ms,
	GError             **error
){
	g_autoptr(GowlGabbrevHandle) h = gowl_gabbrev_lookup_func(name);
	CallBody c;

	if (h == NULL) {
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
		            "c: no function `%s' is registered "
		            "(gowl_gabbrev_register_func)", name);
		return NULL;
	}
	memset(&c, 0, sizeof c);
	c.handle = h;
	c.request = request;
	return guarded(&c, timeout_ms, error);
}
