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
 * gowl-hotkey.c - The hotkey module's process-wide registries.
 *
 * Functions, contexts and sources registered here outlive a config
 * reload and the module being switched off and on: they belong to the
 * process, the way gabbrev's do (src/gabbrev/gowl-gabbrev.c), because
 * the code that registers them -- a C config, a crispy script, cmacs --
 * runs before the module is loaded, or without knowing whether it is.
 *
 * The module reads them on the compositor thread; anyone may register
 * from any thread.  A registered function is held by reference for the
 * length of a call (FuncEntry), so replacing or removing it while it
 * runs only drops the registry's reference, and its destroy notify
 * waits for the call to finish.  Locks are taken inside
 * gowl_fault_guard_hold()/_release(): a watchdog unwind out of a
 * guarded call that was registering something would otherwise leave
 * the lock taken, and the desktop frozen.
 */

#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "gowl-hotkey"

#include "hotkey/gowl-hotkey-private.h"
#include "core/gowl-compositor.h"
#include "core/gowl-core-private.h"
#include "util/gowl-fault-guard.h"
#include <yaml-glib.h>

#include <gio/gio.h>
#include <string.h>

/* ── Calls ─────────────────────────────────────────────────────────── */

struct _GowlHotkeyCall {
	GowlCompositor *compositor;
	gpointer        focused;
	gchar          *id;
	gchar          *trigger;
	gchar          *detail;
	gchar         **args;
	gchar          *result;
};

/**
 * gowl_hotkey_call_new: (skip)
 * @compositor: (nullable): the compositor
 * @focused: (nullable): the focused #GowlClient
 * @id: (nullable): the hotkey, timer or hook that made the call
 * @trigger: (nullable): what kind of thing that is
 * @detail: (nullable): more about it
 * @args: (nullable) (array zero-terminated=1): the arguments
 *
 * Returns: (transfer full): a call; free with gowl_hotkey_call_free()
 */
GowlHotkeyCall *
gowl_hotkey_call_new(
	GowlCompositor      *compositor,
	gpointer             focused,
	const gchar         *id,
	const gchar         *trigger,
	const gchar         *detail,
	const gchar * const *args
){
	GowlHotkeyCall *call;

	call = g_new0(GowlHotkeyCall, 1);
	call->compositor = compositor;
	call->focused = focused;
	call->id = g_strdup(id);
	call->trigger = g_strdup(trigger != NULL ? trigger : "api");
	call->detail = g_strdup(detail);
	call->args = g_strdupv((gchar **)args);
	if (call->args == NULL)
		call->args = g_new0(gchar *, 1);
	return call;
}

/**
 * gowl_hotkey_call_steal_result: (skip)
 * @call: a call
 *
 * Returns: (transfer full) (nullable): what the function gave
 *   gowl_hotkey_call_set_result()
 */
gchar *
gowl_hotkey_call_steal_result(GowlHotkeyCall *call)
{
	g_return_val_if_fail(call != NULL, NULL);
	return g_steal_pointer(&call->result);
}

/**
 * gowl_hotkey_call_free: (skip)
 * @call: (transfer full): a call
 */
void
gowl_hotkey_call_free(GowlHotkeyCall *call)
{
	if (call == NULL)
		return;
	g_free(call->id);
	g_free(call->trigger);
	g_free(call->detail);
	g_strfreev(call->args);
	g_free(call->result);
	g_free(call);
}

/**
 * gowl_hotkey_call_get_compositor:
 * @call: a call
 *
 * Returns: (transfer none) (nullable): the compositor
 */
GowlCompositor *
gowl_hotkey_call_get_compositor(GowlHotkeyCall *call)
{
	g_return_val_if_fail(call != NULL, NULL);
	return call->compositor;
}

/**
 * gowl_hotkey_call_get_focused:
 * @call: a call
 *
 * Returns: (transfer none) (nullable): the #GowlClient that had (or was
 *   getting) the keyboard when the call was made
 */
gpointer
gowl_hotkey_call_get_focused(GowlHotkeyCall *call)
{
	g_return_val_if_fail(call != NULL, NULL);
	return call->focused;
}

/**
 * gowl_hotkey_call_get_id:
 * @call: a call
 *
 * Returns: (transfer none) (nullable): the id of the hotkey, hotstring,
 *   timer or event hook that made the call
 */
const gchar *
gowl_hotkey_call_get_id(GowlHotkeyCall *call)
{
	g_return_val_if_fail(call != NULL, NULL);
	return call->id;
}

/**
 * gowl_hotkey_call_get_trigger:
 * @call: a call
 *
 * Returns: (transfer none): `hotkey', `hotstring', `timer', `event',
 *   `context', `ipc', `dbus' or `api'
 */
const gchar *
gowl_hotkey_call_get_trigger(GowlHotkeyCall *call)
{
	g_return_val_if_fail(call != NULL, NULL);
	return call->trigger;
}

/**
 * gowl_hotkey_call_get_detail:
 * @call: a call
 *
 * Returns: (transfer none) (nullable): for a hotkey its key (`#!t'),
 *   for an event hook the event and its window, for a timer its period
 */
const gchar *
gowl_hotkey_call_get_detail(GowlHotkeyCall *call)
{
	g_return_val_if_fail(call != NULL, NULL);
	return call->detail;
}

/**
 * gowl_hotkey_call_get_args:
 * @call: a call
 *
 * Returns: (transfer none) (array zero-terminated=1): the `args:' of a
 *   `{call: NAME, args: [...]}' action, or of `hotkey-call NAME ARGS'
 */
const gchar * const *
gowl_hotkey_call_get_args(GowlHotkeyCall *call)
{
	g_return_val_if_fail(call != NULL, NULL);
	return (const gchar * const *)call->args;
}

/**
 * gowl_hotkey_call_get_arg:
 * @call: a call
 * @index: which argument, from 0
 *
 * Returns: (transfer none) (nullable): that argument, %NULL past the end
 */
const gchar *
gowl_hotkey_call_get_arg(
	GowlHotkeyCall *call,
	guint           index
){
	g_return_val_if_fail(call != NULL, NULL);
	if (index >= g_strv_length(call->args))
		return NULL;
	return call->args[index];
}

/**
 * gowl_hotkey_call_set_result:
 * @call: a call
 * @result: (nullable): text; copied
 *
 * What the call gives back: `hotkey-call' replies `OK RESULT'.
 */
void
gowl_hotkey_call_set_result(
	GowlHotkeyCall *call,
	const gchar    *result
){
	g_return_if_fail(call != NULL);
	g_free(call->result);
	call->result = g_strdup(result);
}

/* ── The registry ──────────────────────────────────────────────────── */

typedef struct {
	gint                ref;
	GowlHotkeyFunc      func;
	GowlHotkeyPredicate predicate;
	gpointer            data;
	GDestroyNotify      destroy;
} FuncEntry;

static GMutex      registry_lock;
static GHashTable *funcs;         /* name -> FuncEntry* */
static GHashTable *predicates;    /* name -> FuncEntry* */
static GHashTable *booleans;      /* name -> GINT_TO_POINTER(0|1) */
static GPtrArray  *sources;       /* GowlHotkeySource* */
static gint        generation;

static void
bump_generation(void)
{
	g_atomic_int_inc(&generation);
}

/**
 * gowl_hotkey_get_generation: (skip)
 *
 * Returns: a number that changes whenever a function, a context
 *   predicate or a source is added or removed
 */
guint
gowl_hotkey_get_generation(void)
{
	return (guint)g_atomic_int_get(&generation);
}

static FuncEntry *
entry_ref(FuncEntry *e)
{
	g_atomic_int_inc(&e->ref);
	return e;
}

static void
entry_unref(gpointer p)
{
	FuncEntry *e = p;

	if (e == NULL || !g_atomic_int_dec_and_test(&e->ref))
		return;
	if (e->destroy != NULL)
		e->destroy(e->data);
	g_free(e);
}

/* The tables, made on first use.  Called with the lock held. */
static void
ensure_tables(void)
{
	if (funcs == NULL)
		funcs = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
		                              entry_unref);
	if (predicates == NULL)
		predicates = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
		                                   entry_unref);
	if (booleans == NULL)
		booleans = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
		                                 NULL);
	if (sources == NULL)
		sources = g_ptr_array_new_with_free_func(
			(GDestroyNotify)gowl_hotkey_source_free);
}

static void
lock(void)
{
	gowl_fault_guard_hold();
	g_mutex_lock(&registry_lock);
	ensure_tables();
}

static void
unlock(void)
{
	g_mutex_unlock(&registry_lock);
	gowl_fault_guard_release();
}

/* Inserts @e (transfer full) under @name in @table; the old entry, if
 * any, is unreffed AFTER the lock is dropped, since its destroy notify
 * is foreign code that may itself register something. */
static void
table_put(
	GHashTable  **table,
	const gchar  *name,
	FuncEntry    *e
){
	FuncEntry *old = NULL;

	lock();
	if (g_hash_table_steal_extended(*table, name, NULL, (gpointer *)&old))
		g_hash_table_remove(*table, name);
	g_hash_table_insert(*table, g_strdup(name), e);
	unlock();
	entry_unref(old);
	bump_generation();
}

static gboolean
table_remove(
	GHashTable  **table,
	const gchar  *name
){
	FuncEntry *old = NULL;
	gpointer key = NULL;
	gboolean found;

	lock();
	found = g_hash_table_steal_extended(*table, name, &key, (gpointer *)&old);
	unlock();
	g_free(key);
	entry_unref(old);
	if (found)
		bump_generation();
	return found;
}

static FuncEntry *
table_lookup(
	GHashTable  **table,
	const gchar  *name
){
	FuncEntry *e;

	lock();
	e = g_hash_table_lookup(*table, name);
	if (e != NULL)
		entry_ref(e);
	unlock();
	return e;
}

static gint
compare_names(
	gconstpointer a,
	gconstpointer b
){
	return g_strcmp0(*(const gchar * const *)a, *(const gchar * const *)b);
}

static GStrv
table_keys(GHashTable **table)
{
	GPtrArray *out;
	GHashTableIter it;
	gpointer key;

	out = g_ptr_array_new();
	lock();
	g_hash_table_iter_init(&it, *table);
	while (g_hash_table_iter_next(&it, &key, NULL))
		g_ptr_array_add(out, g_strdup(key));
	unlock();
	g_ptr_array_sort(out, compare_names);
	g_ptr_array_add(out, NULL);
	return (GStrv)g_ptr_array_free(out, FALSE);
}

/* ── Functions ─────────────────────────────────────────────────────── */

/**
 * gowl_hotkey_register_func:
 * @name: the name a `{call: NAME}' action uses
 * @func: (scope notified): the function
 * @user_data: (closure func): passed to @func
 * @destroy: (nullable): frees @user_data when the function is replaced
 *   or unregistered (after any call still running has returned)
 *
 * Names a C function for hotkeys to call, replacing any of the same
 * name.  From a C config or a crispy hotkey script:
 *
 * |[<!-- language="C" -->
 * static void
 * centre_focused(GowlHotkeyCall *call, gpointer data)
 * {
 *     g_free(gowl_hotkey_command(gowl_hotkey_call_get_compositor(call),
 *                                "action center"));
 * }
 *
 * gowl_hotkey_register_func("centre", centre_focused, NULL, NULL);
 * ]|
 *
 * and in YAML: `- {key: "#c", do: {call: centre}}'.
 */
void
gowl_hotkey_register_func(
	const gchar    *name,
	GowlHotkeyFunc  func,
	gpointer        user_data,
	GDestroyNotify  destroy
){
	FuncEntry *e;

	g_return_if_fail(name != NULL && *name != '\0');
	g_return_if_fail(func != NULL);

	e = g_new0(FuncEntry, 1);
	e->ref = 1;
	e->func = func;
	e->data = user_data;
	e->destroy = destroy;
	table_put(&funcs, name, e);
}

/**
 * gowl_hotkey_unregister_func:
 * @name: a name given to gowl_hotkey_register_func()
 *
 * Returns: %TRUE when there was one
 */
gboolean
gowl_hotkey_unregister_func(const gchar *name)
{
	g_return_val_if_fail(name != NULL, FALSE);
	return table_remove(&funcs, name);
}

/**
 * gowl_hotkey_list_funcs:
 *
 * Returns: (transfer full): the registered function names
 */
GStrv
gowl_hotkey_list_funcs(void)
{
	return table_keys(&funcs);
}

/**
 * gowl_hotkey_has_func: (skip)
 * @name: a function name
 *
 * Returns: %TRUE when a function of that name is registered
 */
gboolean
gowl_hotkey_has_func(const gchar *name)
{
	FuncEntry *e;

	if (name == NULL)
		return FALSE;
	e = table_lookup(&funcs, name);
	entry_unref(e);
	return e != NULL;
}

typedef struct {
	FuncEntry      *entry;
	GowlHotkeyCall *call;
	gboolean        result;
} Invoke;

static void
invoke_func_body(gpointer data)
{
	Invoke *iv = data;

	iv->entry->func(iv->call, iv->entry->data);
}

static void
invoke_predicate_body(gpointer data)
{
	Invoke *iv = data;

	iv->result = iv->entry->predicate(iv->call, iv->entry->data);
}

/**
 * gowl_hotkey_invoke_func: (skip)
 * @name: a registered function
 * @call: its call
 * @timeout_ms: its budget
 * @error: an unknown name, or the signal it faulted with
 *
 * Returns: %TRUE when it ran and returned
 */
gboolean
gowl_hotkey_invoke_func(
	const gchar     *name,
	GowlHotkeyCall  *call,
	guint            timeout_ms,
	GError         **error
){
	Invoke iv;
	gint signo = 0;
	gboolean ok;

	g_return_val_if_fail(name != NULL && call != NULL, FALSE);

	memset(&iv, 0, sizeof iv);
	iv.entry = table_lookup(&funcs, name);
	if (iv.entry == NULL) {
		g_set_error(error, GOWL_HOTKEY_ERROR, GOWL_HOTKEY_ERROR_NOT_FOUND,
		            "no function `%s' is registered", name);
		return FALSE;
	}
	iv.call = call;
	ok = gowl_fault_guard_call(invoke_func_body, &iv, timeout_ms, &signo,
	                           "a hotkey function");
	if (!ok)
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
		            "function `%s' %s and was stopped", name,
		            gowl_fault_guard_signal_name(signo));
	entry_unref(iv.entry);
	return ok;
}

/* ── Contexts ──────────────────────────────────────────────────────── */

/**
 * gowl_hotkey_register_context:
 * @name: the name a hotkey's `context:' uses
 * @predicate: (scope notified): whether a hotkey naming it may fire now
 * @user_data: (closure predicate): passed to @predicate
 * @destroy: (nullable): frees @user_data
 *
 * Names a C predicate as a context, replacing any of the same name.  A
 * context named in the YAML (`contexts: {NAME: FILTER}') wins over a
 * predicate of the same name.
 */
void
gowl_hotkey_register_context(
	const gchar         *name,
	GowlHotkeyPredicate  predicate,
	gpointer             user_data,
	GDestroyNotify       destroy
){
	FuncEntry *e;

	g_return_if_fail(name != NULL && *name != '\0');
	g_return_if_fail(predicate != NULL);

	e = g_new0(FuncEntry, 1);
	e->ref = 1;
	e->predicate = predicate;
	e->data = user_data;
	e->destroy = destroy;
	table_put(&predicates, name, e);
}

/**
 * gowl_hotkey_unregister_context:
 * @name: a name given to gowl_hotkey_register_context()
 *
 * Returns: %TRUE when there was one
 */
gboolean
gowl_hotkey_unregister_context(const gchar *name)
{
	g_return_val_if_fail(name != NULL, FALSE);
	return table_remove(&predicates, name);
}

/**
 * gowl_hotkey_set_context:
 * @name: a context name
 * @value: whether hotkeys naming it may fire
 *
 * A context that is a plain boolean someone else keeps up to date.
 * This is how cmacs's Elisp contexts work: Lisp evaluates the
 * predicate on its own thread when the focus or a title changes and
 * sets the answer here, so deciding a key never waits for Lisp.  Any
 * program can do the same over IPC with `hotkey-context-set NAME 1'.
 */
void
gowl_hotkey_set_context(
	const gchar *name,
	gboolean     value
){
	g_return_if_fail(name != NULL && *name != '\0');
	lock();
	g_hash_table_replace(booleans, g_strdup(name),
	                     GINT_TO_POINTER(value ? 1 : 0));
	unlock();
}

/**
 * gowl_hotkey_get_context:
 * @name: a context name
 * @value: (out) (optional): its value
 *
 * Returns: %TRUE when a boolean context of that name has been set
 */
gboolean
gowl_hotkey_get_context(
	const gchar *name,
	gboolean    *value
){
	gpointer v = NULL;
	gboolean found;

	g_return_val_if_fail(name != NULL, FALSE);
	lock();
	found = g_hash_table_lookup_extended(booleans, name, NULL, &v);
	unlock();
	if (value != NULL)
		*value = found && GPOINTER_TO_INT(v) != 0;
	return found;
}

/**
 * gowl_hotkey_clear_context:
 * @name: a context name
 *
 * Forgets a boolean context; hotkeys naming it no longer fire.
 *
 * Returns: %TRUE when there was one
 */
gboolean
gowl_hotkey_clear_context(const gchar *name)
{
	gboolean found;

	g_return_val_if_fail(name != NULL, FALSE);
	lock();
	found = g_hash_table_remove(booleans, name);
	unlock();
	return found;
}

/**
 * gowl_hotkey_list_contexts:
 *
 * Returns: (transfer full): the registered predicate and boolean
 *   context names
 */
GStrv
gowl_hotkey_list_contexts(void)
{
	GPtrArray *out;
	GHashTableIter it;
	gpointer key;

	out = g_ptr_array_new();
	lock();
	g_hash_table_iter_init(&it, predicates);
	while (g_hash_table_iter_next(&it, &key, NULL))
		g_ptr_array_add(out, g_strdup(key));
	g_hash_table_iter_init(&it, booleans);
	while (g_hash_table_iter_next(&it, &key, NULL))
		if (!g_hash_table_contains(predicates, key))
			g_ptr_array_add(out, g_strdup(key));
	unlock();
	g_ptr_array_add(out, NULL);
	return (GStrv)g_ptr_array_free(out, FALSE);
}

/**
 * gowl_hotkey_eval_context: (skip)
 * @name: a context name
 * @call: the call the predicate is given
 * @timeout_ms: a predicate's budget
 * @error: a fault
 *
 * Returns: 1 when the context holds, 0 when not, -1 when nothing of
 *   that name is registered or set
 */
gint
gowl_hotkey_eval_context(
	const gchar     *name,
	GowlHotkeyCall  *call,
	guint            timeout_ms,
	GError         **error
){
	Invoke iv;
	gint signo = 0;
	gboolean value;

	g_return_val_if_fail(name != NULL, -1);

	memset(&iv, 0, sizeof iv);
	iv.entry = table_lookup(&predicates, name);
	if (iv.entry == NULL) {
		if (gowl_hotkey_get_context(name, &value))
			return value ? 1 : 0;
		return -1;
	}
	iv.call = call;
	if (!gowl_fault_guard_call(invoke_predicate_body, &iv, timeout_ms,
	                           &signo, "a hotkey context")) {
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
		            "context `%s' %s and was stopped", name,
		            gowl_fault_guard_signal_name(signo));
		iv.result = FALSE;
	}
	entry_unref(iv.entry);
	return iv.result ? 1 : 0;
}

/* ── Sources ───────────────────────────────────────────────────────── */

/**
 * gowl_hotkey_source_free: (skip)
 * @source: (transfer full): a source
 */
void
gowl_hotkey_source_free(GowlHotkeySource *source)
{
	if (source == NULL)
		return;
	g_free(source->id);
	g_free(source->yaml);
	g_free(source);
}

/**
 * gowl_hotkey_add_yaml:
 * @id: a name for the section; adding again under the same name
 *   replaces it
 * @yaml: a `hotkey:' section's contents
 * @error: return location for a parse error
 *
 * Adds hotkeys (and remaps, modes, timers, hooks, functions...) for the
 * rest of the process: they survive a config reload, and load whenever
 * the module does.  The text is parsed here, so a syntax error is the
 * caller's; what it means -- an unknown key name, an action that does
 * not exist -- is checked when the module loads it and reported in
 * `hotkey-status', like a problem in the config's own section.
 *
 * |[<!-- language="C" -->
 * gowl_hotkey_add_yaml("my-layer",
 *     "hotkeys:\n"
 *     "  - {key: \"CapsLock & h\", do: {send: \"{Left}\"}}\n"
 *     "  - {key: \"CapsLock & l\", do: {send: \"{Right}\"}}\n", &error);
 * ]|
 *
 * Returns: %TRUE when @yaml parsed to a mapping and was added
 */
gboolean
gowl_hotkey_add_yaml(
	const gchar  *id,
	const gchar  *yaml,
	GError      **error
){
	g_autoptr(YamlParser) parser = NULL;
	GowlHotkeySource *s;
	YamlNode *root;
	GError *local = NULL;
	gboolean replaced = FALSE;
	guint i;

	g_return_val_if_fail(id != NULL && *id != '\0', FALSE);
	g_return_val_if_fail(yaml != NULL, FALSE);

	/* Always with an error location: yaml-glib dereferences a NULL one
	   on malformed input */
	parser = yaml_parser_new();
	if (!yaml_parser_load_from_data(parser, yaml, -1, &local)) {
		g_propagate_error(error, local);
		return FALSE;
	}
	root = yaml_parser_get_root(parser);
	if (root != NULL && yaml_node_get_node_type(root) != YAML_NODE_MAPPING) {
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
		            "a hotkey section is a mapping (hotkeys:, remaps:, "
		            "modes:, ...)");
		return FALSE;
	}

	s = g_new0(GowlHotkeySource, 1);
	s->id = g_strdup(id);
	s->yaml = g_strdup(yaml);
	lock();
	for (i = 0; i < sources->len; i++) {
		GowlHotkeySource *old = g_ptr_array_index(sources, i);

		if (g_strcmp0(old->id, id) == 0) {
			gowl_hotkey_source_free(old);
			g_ptr_array_index(sources, i) = s;
			replaced = TRUE;
			break;
		}
	}
	if (!replaced)
		g_ptr_array_add(sources, s);
	unlock();
	bump_generation();
	return TRUE;
}

/**
 * gowl_hotkey_remove_yaml:
 * @id: a name given to gowl_hotkey_add_yaml()
 *
 * Returns: %TRUE when there was one
 */
gboolean
gowl_hotkey_remove_yaml(const gchar *id)
{
	gboolean found = FALSE;
	guint i;

	g_return_val_if_fail(id != NULL, FALSE);
	lock();
	for (i = 0; i < sources->len; i++) {
		GowlHotkeySource *s = g_ptr_array_index(sources, i);

		if (g_strcmp0(s->id, id) == 0) {
			g_ptr_array_remove_index(sources, i);
			found = TRUE;
			break;
		}
	}
	unlock();
	if (found)
		bump_generation();
	return found;
}

/**
 * gowl_hotkey_clear_sources:
 *
 * Forgets every section added with gowl_hotkey_add_yaml() and every
 * hotkey made with gowl_hotkey_define().  Registered functions and
 * contexts stay.
 */
void
gowl_hotkey_clear_sources(void)
{
	lock();
	g_ptr_array_set_size(sources, 0);
	unlock();
	bump_generation();
}

/**
 * gowl_hotkey_dup_sources: (skip)
 *
 * Returns: (transfer full) (element-type GowlHotkeySource): a copy of
 *   the sources, in the order they were added
 */
GPtrArray *
gowl_hotkey_dup_sources(void)
{
	GPtrArray *out;
	guint i;

	out = g_ptr_array_new_with_free_func(
		(GDestroyNotify)gowl_hotkey_source_free);
	lock();
	for (i = 0; i < sources->len; i++) {
		GowlHotkeySource *s = g_ptr_array_index(sources, i);
		GowlHotkeySource *c = g_new0(GowlHotkeySource, 1);

		c->id = g_strdup(s->id);
		c->yaml = g_strdup(s->yaml);
		g_ptr_array_add(out, c);
	}
	unlock();
	return out;
}

/* ── One-line hotkeys ──────────────────────────────────────────────── */

/* A double-quoted YAML scalar: the key text may hold anything, and an
 * unquoted `#' would start a comment. */
static gchar *
yaml_quote(const gchar *s)
{
	GString *out = g_string_new("\"");

	for (; *s != '\0'; s++) {
		if (*s == '"' || *s == '\\')
			g_string_append_c(out, '\\');
		if (*s == '\n')
			g_string_append(out, "\\n");
		else
			g_string_append_c(out, *s);
	}
	g_string_append_c(out, '"');
	return g_string_free(out, FALSE);
}

/**
 * gowl_hotkey_define:
 * @id: the hotkey's id, also the name its function is registered as
 * @key: the hotkey, in either notation (`#!t', `Super+Alt+t')
 * @func: (scope notified): what it runs
 * @user_data: (closure func): passed to @func
 * @destroy: (nullable): frees @user_data
 * @error: return location for an error -- @key is checked here
 *
 * "This key runs this C function", in one call: registers @func as
 * @id and adds the source `{id: ID, key: KEY, do: {call: ID}}' under
 * the source id `c:ID'.  For a hotkey with options -- a context, a
 * mode, a double tap -- register the function and write the YAML with
 * gowl_hotkey_add_yaml().
 *
 * |[<!-- language="C" -->
 * gowl_hotkey_define("say-hi", "#!h", say_hi, NULL, NULL, &error);
 * ]|
 *
 * Returns: %TRUE when @key parsed and the hotkey was added
 */
gboolean
gowl_hotkey_define(
	const gchar     *id,
	const gchar     *key,
	GowlHotkeyFunc   func,
	gpointer         user_data,
	GDestroyNotify   destroy,
	GError         **error
){
	GowlHotkeySpec spec;
	g_autofree gchar *qid = NULL;
	g_autofree gchar *qkey = NULL;
	g_autofree gchar *yaml = NULL;
	g_autofree gchar *source = NULL;

	g_return_val_if_fail(id != NULL && *id != '\0', FALSE);
	g_return_val_if_fail(key != NULL, FALSE);
	g_return_val_if_fail(func != NULL, FALSE);

	if (!gowl_hotkey_spec_parse(key, &spec, error)) {
		if (destroy != NULL)
			destroy(user_data);
		return FALSE;
	}
	qid = yaml_quote(id);
	qkey = yaml_quote(key);
	yaml = g_strdup_printf("hotkeys:\n  - {id: %s, key: %s, do: {call: %s}}\n",
	                       qid, qkey, qid);
	source = g_strdup_printf("c:%s", id);
	gowl_hotkey_register_func(id, func, user_data, destroy);
	if (!gowl_hotkey_add_yaml(source, yaml, error)) {
		gowl_hotkey_unregister_func(id);
		return FALSE;
	}
	return TRUE;
}

/**
 * gowl_hotkey_undefine:
 * @id: an id given to gowl_hotkey_define()
 *
 * Returns: %TRUE when there was one
 */
gboolean
gowl_hotkey_undefine(const gchar *id)
{
	g_autofree gchar *source = NULL;
	gboolean found;

	g_return_val_if_fail(id != NULL, FALSE);
	source = g_strdup_printf("c:%s", id);
	found = gowl_hotkey_remove_yaml(source);
	return gowl_hotkey_unregister_func(id) || found;
}

/* ── Commands to the running module ────────────────────────────────── */

/* `OK ...' from a command line. */
static gboolean
command_ok(
	GowlCompositor *compositor,
	const gchar    *line
){
	g_autofree gchar *reply = NULL;

	reply = gowl_compositor_run_command(compositor, line);
	return reply != NULL && g_str_has_prefix(reply, "OK");
}

/**
 * gowl_hotkey_command:
 * @compositor: the compositor
 * @line: a command line (`hotkey-status', `action center', `view 4')
 *
 * Runs any gowl command line -- the core's and every module's -- the
 * way an IPC client would, for C code that has a #GowlCompositor.
 *
 * Returns: (transfer full) (nullable): the reply, %NULL for an unknown
 *   command
 */
gchar *
gowl_hotkey_command(
	GowlCompositor *compositor,
	const gchar    *line
){
	g_return_val_if_fail(GOWL_IS_COMPOSITOR(compositor), NULL);
	g_return_val_if_fail(line != NULL, NULL);
	return gowl_compositor_ipc_command(compositor, line);
}

/**
 * gowl_hotkey_send:
 * @compositor: the compositor
 * @keys: keys in AutoHotkey's Send syntax: `^c', `{Enter}',
 *   `{Tab 3}', `{Blind}{Down}', `{Text}literal'
 *
 * Types into the focused window through the hotkey module.
 *
 * Returns: %TRUE when the module took it (it is typed a loop turn
 *   later, never inside the call)
 */
gboolean
gowl_hotkey_send(
	GowlCompositor *compositor,
	const gchar    *keys
){
	g_autofree gchar *line = NULL;

	g_return_val_if_fail(GOWL_IS_COMPOSITOR(compositor), FALSE);
	g_return_val_if_fail(keys != NULL, FALSE);
	line = g_strdup_printf("hotkey-send %s", keys);
	return command_ok(compositor, line);
}

/**
 * gowl_hotkey_send_text:
 * @compositor: the compositor
 * @text: literal text; nothing in it is a key name
 *
 * Returns: %TRUE when the module took it
 */
gboolean
gowl_hotkey_send_text(
	GowlCompositor *compositor,
	const gchar    *text
){
	g_autofree gchar *line = NULL;

	g_return_val_if_fail(GOWL_IS_COMPOSITOR(compositor), FALSE);
	g_return_val_if_fail(text != NULL, FALSE);
	line = g_strdup_printf("hotkey-send-text %s", text);
	return command_ok(compositor, line);
}

/**
 * gowl_hotkey_set_mode:
 * @compositor: the compositor
 * @mode: (nullable): a mode name, or %NULL / "default" to leave modes
 *
 * Returns: %TRUE when the module took it
 */
gboolean
gowl_hotkey_set_mode(
	GowlCompositor *compositor,
	const gchar    *mode
){
	g_autofree gchar *line = NULL;

	g_return_val_if_fail(GOWL_IS_COMPOSITOR(compositor), FALSE);
	line = g_strdup_printf("hotkey-mode %s", mode != NULL ? mode : "default");
	return command_ok(compositor, line);
}

/**
 * gowl_hotkey_set_suspended:
 * @compositor: the compositor
 * @suspended: %TRUE to suspend every hotkey not exempt from it
 *
 * Returns: %TRUE when the module took it
 */
gboolean
gowl_hotkey_set_suspended(
	GowlCompositor *compositor,
	gboolean        suspended
){
	g_return_val_if_fail(GOWL_IS_COMPOSITOR(compositor), FALSE);
	return command_ok(compositor, suspended ? "hotkey-suspend on"
	                                        : "hotkey-suspend off");
}

/**
 * gowl_hotkey_call_function:
 * @compositor: the compositor
 * @name: a function -- `functions:' in the YAML, a registered C one,
 *   or (under cmacs) an Elisp one
 * @args: (nullable) (array zero-terminated=1): its arguments
 *
 * Returns: (transfer full) (nullable): the module's reply, `OK RESULT'
 *   or `ERROR ...'; %NULL without the module
 */
gchar *
gowl_hotkey_call_function(
	GowlCompositor      *compositor,
	const gchar         *name,
	const gchar * const *args
){
	g_autoptr(GString) line = NULL;
	g_autofree gchar *qname = NULL;
	guint i;

	g_return_val_if_fail(GOWL_IS_COMPOSITOR(compositor), NULL);
	g_return_val_if_fail(name != NULL, NULL);

	qname = g_shell_quote(name);
	line = g_string_new("hotkey-call ");
	g_string_append(line, qname);
	for (i = 0; args != NULL && args[i] != NULL; i++) {
		g_autofree gchar *q = g_shell_quote(args[i]);

		g_string_append_c(line, ' ');
		g_string_append(line, q);
	}
	return gowl_compositor_run_command(compositor, line->str);
}
