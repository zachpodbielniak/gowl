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
 * gowl-hotkey-script.c - Building, opening and calling hotkey C.
 *
 * The compiler and its flags are the macro module's and gabbrev's (see
 * modules/gabbrev/gowl-gabbrev-cvar.c): the in-tree headers in a
 * development build, pkg-config's otherwise, plus CRISPY_PARAMS from the
 * source.  The code is the user's own, on the user's own desktop, and
 * its flags are trusted the same way.
 */

#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "gowl-hotkey"

#include "gowl-hotkey-script.h"
#include "util/gowl-fault-guard.h"

#include <crispy.h>
#include <gio/gio.h>
#include <string.h>

/* Opening a build runs its constructors: a short budget */
#define OPEN_TIMEOUT_MS (2000)

void
hk_c_spec_clear(HkCSpec *spec)
{
	g_clear_pointer(&spec->code, g_free);
	g_clear_pointer(&spec->file, g_free);
	g_clear_pointer(&spec->cflags, g_free);
}

/* What identifies a build: the code and flags, or the file's path
 * (whose content the cache checks when it builds). */
gchar *
hk_c_spec_key(const HkCSpec *spec)
{
	g_autofree gchar *what = g_strdup_printf("%s\n%s",
		spec->file != NULL ? spec->file : spec->code,
		spec->cflags != NULL ? spec->cflags : "");

	return g_compute_checksum_for_string(G_CHECKSUM_SHA256, what, -1);
}

/**
 * hk_c_full_source: (skip)
 * @spec: a build
 * @error: why the source cannot be had
 *
 * Returns: (transfer full) (nullable): the translation unit
 */
gchar *
hk_c_full_source(
	const HkCSpec  *spec,
	GError        **error
){
	gchar *text = NULL;

	if (spec->file != NULL) {
		if (!g_file_get_contents(spec->file, &text, NULL, error))
			return NULL;
		return text;
	}
	/* A body: the function around it.  #line points a compile error into
	   the snippet, not the wrapper. */
	return g_strdup_printf(
		"#include <gowl/gowl.h>\n"
		"#define ARG(n) gowl_hotkey_call_get_arg(call, (n))\n"
		"G_MODULE_EXPORT void " HK_C_ACTION_SYMBOL "(GowlHotkeyCall *call);\n"
		"G_MODULE_EXPORT void\n" HK_C_ACTION_SYMBOL "(GowlHotkeyCall *call)\n"
		"{\n"
		"\tGowlCompositor *comp = gowl_hotkey_call_get_compositor(call);\n"
		"\tGowlClient *focused = (GowlClient *)gowl_hotkey_call_get_focused(call);\n"
		"\tconst gchar * const *args = gowl_hotkey_call_get_args(call);\n"
		"\t(void)comp; (void)focused; (void)args;\n"
		"#line 1 \"hotkey-c-action\"\n"
		"%s\n}\n", spec->code != NULL ? spec->code : "");
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
#ifdef HOTKEY_DEV_INCLUDE
	if (g_file_test(HOTKEY_DEV_INCLUDE "/gowl/gowl.h", G_FILE_TEST_EXISTS))
		return g_strdup_printf("-I%s -I%s/gowl -DWLR_USE_UNSTABLE %s",
		                       HOTKEY_DEV_INCLUDE, HOTKEY_DEV_INCLUDE, deps);
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
 * hk_c_build: (skip)
 * @spec: what to build
 * @cache_dir: where builds (and written-out snippets) are kept
 * @error: a one-line compile error
 *
 * Any thread.  An unchanged source is not rebuilt.
 *
 * Returns: (transfer full) (nullable): the shared object's path
 */
gchar *
hk_c_build(
	const HkCSpec  *spec,
	const gchar    *cache_dir,
	GError        **error
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

	source = hk_c_full_source(spec, error);
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

	/* A file builds from where it is; a body is written out first */
	if (spec->file != NULL) {
		src_path = g_strdup(spec->file);
	} else {
		g_autofree gchar *name = g_strdup_printf("hotkey-%s.c", hash);

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
			                     "did not compile: %s", flat);
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
 * hk_c_open: (skip)
 * @so_path: a build
 * @error: why it did not open
 *
 * Compositor thread.  The build stays loaded for the life of the
 * process (a rebuilt one is a new file).
 *
 * Returns: (nullable) (transfer none): the module
 */
GModule *
hk_c_open(
	const gchar  *so_path,
	GError      **error
){
	OpenCall o;
	gint signo = 0;

	o.so_path = so_path;
	o.module = NULL;
	if (!gowl_fault_guard_call(open_body, &o, OPEN_TIMEOUT_MS, &signo,
	                           "opening hotkey C")) {
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
	return o.module;
}

typedef struct {
	HkCActionFunc   action;
	HkCInitFunc     init;
	HkCFiniFunc     fini;
	GowlHotkeyCall *call;
	gboolean        ok;
} CallBody;

static void
call_body(gpointer data)
{
	CallBody *c = data;

	if (c->action != NULL) {
		c->action(c->call);
		c->ok = TRUE;
	} else if (c->init != NULL) {
		c->ok = c->init();
	} else if (c->fini != NULL) {
		c->fini();
		c->ok = TRUE;
	}
}

static gboolean
guarded(
	CallBody     *c,
	guint         timeout_ms,
	const gchar  *what,
	GError      **error
){
	gint signo = 0;

	if (!gowl_fault_guard_call(call_body, c, timeout_ms, &signo, what)) {
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
		            "%s %s and was stopped", what,
		            gowl_fault_guard_signal_name(signo));
		return FALSE;
	}
	return TRUE;
}

/**
 * hk_c_call_action: (skip)
 * @func: an opened action
 * @call: its call
 * @timeout_ms: its budget
 * @error: a fault
 *
 * Returns: %TRUE when it returned
 */
gboolean
hk_c_call_action(
	HkCActionFunc    func,
	GowlHotkeyCall  *call,
	guint            timeout_ms,
	GError         **error
){
	CallBody c;

	memset(&c, 0, sizeof c);
	c.action = func;
	c.call = call;
	return guarded(&c, timeout_ms, "a hotkey C action", error);
}

/**
 * hk_c_call_init: (skip)
 * @func: a script's init
 * @timeout_ms: its budget
 * @error: a fault, or the init's FALSE
 *
 * Returns: %TRUE when it returned TRUE
 */
gboolean
hk_c_call_init(
	HkCInitFunc   func,
	guint         timeout_ms,
	GError      **error
){
	CallBody c;

	memset(&c, 0, sizeof c);
	c.init = func;
	if (!guarded(&c, timeout_ms, "a hotkey script's init", error))
		return FALSE;
	if (!c.ok)
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
		            HK_C_INIT_SYMBOL "() returned FALSE");
	return c.ok;
}

/**
 * hk_c_call_fini: (skip)
 * @func: a script's fini
 * @timeout_ms: its budget
 * @error: a fault
 *
 * Returns: %TRUE when it returned
 */
gboolean
hk_c_call_fini(
	HkCFiniFunc   func,
	guint         timeout_ms,
	GError      **error
){
	CallBody c;

	memset(&c, 0, sizeof c);
	c.fini = func;
	return guarded(&c, timeout_ms, "a hotkey script's fini", error);
}
