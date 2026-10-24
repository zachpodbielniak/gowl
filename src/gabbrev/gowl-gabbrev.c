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
 * gowl-gabbrev.c - Variable requests, the two registries, and the
 * process-wide sources of the text expander.
 *
 * The three locks here are taken on the compositor thread and on any
 * thread a provider answers from, so every one is wrapped in
 * gowl_fault_guard_hold()/release(): a watchdog firing inside would
 * otherwise leave the lock held and the next keystroke would wait on it
 * for ever (see docs/macros.org, "The watchdog").
 */

#include "gabbrev/gowl-gabbrev.h"
#include "gabbrev/gowl-gabbrev-private.h"
#include "util/gowl-fault-guard.h"

#include <yaml-glib.h>
#include <string.h>

/* ── Requests ─────────────────────────────────────────────────────────── */

struct _GowlGabbrevRequest {
	gint                    ref_count;
	guint64                 id;
	gchar                  *var_name;
	gchar                  *var_type;
	GHashTable             *params;
	gchar                  *params_json;
	GHashTable             *vars;
	gchar                  *trigger;
	gchar                  *app_id;
	gchar                  *title;

	GMutex                  lock;
	gboolean                answered;
	gboolean                cancelled;
	gchar                  *value;
	gchar                  *error;
	GowlGabbrevRequestDone  done;
	gpointer                done_data;
	GDestroyNotify          done_destroy;
};

/* id -> request (no reference): what return_by_id() can reach.  An
   entry lives from creation until the request is answered, cancelled
   or freed, whichever is first. */
static GMutex      pending_lock;
static GHashTable *pending;
static guint64     next_id = 1;

G_DEFINE_BOXED_TYPE(GowlGabbrevRequest, gowl_gabbrev_request,
                    gowl_gabbrev_request_ref, gowl_gabbrev_request_unref)

static void
pending_remove(guint64 id)
{
	gowl_fault_guard_hold();
	g_mutex_lock(&pending_lock);
	if (pending != NULL)
		g_hash_table_remove(pending, &id);
	g_mutex_unlock(&pending_lock);
	gowl_fault_guard_release();
}

/* A table that is never NULL, so getters need no NULL checks. */
static GHashTable *
table_or_empty(GHashTable *t)
{
	if (t != NULL)
		return g_hash_table_ref(t);
	return g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
}

/**
 * gowl_gabbrev_request_new: (skip)
 * @info: what is being asked for
 * @done: (nullable): told once, from the answering thread
 * @user_data: (nullable): for @done
 * @destroy: (nullable): frees @user_data when the request is freed --
 *   NOT when it is answered, so @user_data outlives a done notify that
 *   is still running on another thread when the request is cancelled
 *
 * Returns: (transfer full): a new request, reachable by its id until
 *   it is answered or cancelled
 */
GowlGabbrevRequest *
gowl_gabbrev_request_new(
	const GowlGabbrevRequestInfo *info,
	GowlGabbrevRequestDone        done,
	gpointer                      user_data,
	GDestroyNotify                destroy
){
	GowlGabbrevRequest *r;

	g_return_val_if_fail(info != NULL, NULL);

	r = g_new0(GowlGabbrevRequest, 1);
	r->ref_count = 1;
	r->var_name = g_strdup(info->var_name != NULL ? info->var_name : "");
	r->var_type = g_strdup(info->var_type != NULL ? info->var_type : "");
	r->params = table_or_empty(info->params);
	r->params_json = g_strdup(info->params_json != NULL
	                          ? info->params_json : "{}");
	r->vars = table_or_empty(info->vars);
	r->trigger = g_strdup(info->trigger != NULL ? info->trigger : "");
	r->app_id = g_strdup(info->app_id != NULL ? info->app_id : "");
	r->title = g_strdup(info->title != NULL ? info->title : "");
	r->done = done;
	r->done_data = user_data;
	r->done_destroy = destroy;
	g_mutex_init(&r->lock);

	gowl_fault_guard_hold();
	g_mutex_lock(&pending_lock);
	if (pending == NULL)
		pending = g_hash_table_new(g_int64_hash, g_int64_equal);
	r->id = next_id++;
	g_hash_table_insert(pending, &r->id, r);
	g_mutex_unlock(&pending_lock);
	gowl_fault_guard_release();
	return r;
}

/**
 * gowl_gabbrev_request_ref:
 * @request: a request
 *
 * Returns: (transfer full): @request
 */
GowlGabbrevRequest *
gowl_gabbrev_request_ref(GowlGabbrevRequest *request)
{
	g_return_val_if_fail(request != NULL, NULL);
	g_atomic_int_inc(&request->ref_count);
	return request;
}

/**
 * gowl_gabbrev_request_unref:
 * @request: (transfer full): a request
 */
void
gowl_gabbrev_request_unref(GowlGabbrevRequest *request)
{
	if (request == NULL)
		return;
	if (!g_atomic_int_dec_and_test(&request->ref_count))
		return;
	pending_remove(request->id);
	g_hash_table_unref(request->params);
	g_hash_table_unref(request->vars);
	g_free(request->var_name);
	g_free(request->var_type);
	g_free(request->params_json);
	g_free(request->trigger);
	g_free(request->app_id);
	g_free(request->title);
	g_free(request->value);
	g_free(request->error);
	g_mutex_clear(&request->lock);
	if (request->done_destroy != NULL && request->done_data != NULL)
		request->done_destroy(request->done_data);
	g_free(request);
}

/**
 * gowl_gabbrev_request_get_id:
 * @request: a request
 *
 * The number gowl_gabbrev_request_return_by_id() answers it by: a
 * provider that hands the work to another process or language keeps
 * this rather than a pointer.
 *
 * Returns: the request's id, unique for the life of the process
 */
guint64
gowl_gabbrev_request_get_id(GowlGabbrevRequest *request)
{
	g_return_val_if_fail(request != NULL, 0);
	return request->id;
}

/**
 * gowl_gabbrev_request_get_var_name:
 * @request: a request
 *
 * Returns: (transfer none): the variable's name in its match
 */
const gchar *
gowl_gabbrev_request_get_var_name(GowlGabbrevRequest *request)
{
	g_return_val_if_fail(request != NULL, NULL);
	return request->var_name;
}

/**
 * gowl_gabbrev_request_get_var_type:
 * @request: a request
 *
 * Returns: (transfer none): the variable's `type:'
 */
const gchar *
gowl_gabbrev_request_get_var_type(GowlGabbrevRequest *request)
{
	g_return_val_if_fail(request != NULL, NULL);
	return request->var_type;
}

/**
 * gowl_gabbrev_request_get_param:
 * @request: a request
 * @key: a key of the variable's `params:'
 *
 * Scalar params as written, after `{{var}}' substitution; a list is its
 * elements joined with newlines; a mapping is its JSON text.  Every
 * param is also in gowl_gabbrev_request_get_params_json(), exactly.
 *
 * Returns: (transfer none) (nullable): the value, %NULL when absent
 */
const gchar *
gowl_gabbrev_request_get_param(
	GowlGabbrevRequest *request,
	const gchar        *key
){
	g_return_val_if_fail(request != NULL, NULL);
	g_return_val_if_fail(key != NULL, NULL);
	return g_hash_table_lookup(request->params, key);
}

/**
 * gowl_gabbrev_request_get_params:
 * @request: a request
 *
 * Returns: (transfer none) (element-type utf8 utf8): every param as
 *   gowl_gabbrev_request_get_param() gives it
 */
GHashTable *
gowl_gabbrev_request_get_params(GowlGabbrevRequest *request)
{
	g_return_val_if_fail(request != NULL, NULL);
	return request->params;
}

/**
 * gowl_gabbrev_request_get_params_json:
 * @request: a request
 *
 * Returns: (transfer none): the `params:' mapping as JSON, `{}' when
 *   it had none
 */
const gchar *
gowl_gabbrev_request_get_params_json(GowlGabbrevRequest *request)
{
	g_return_val_if_fail(request != NULL, NULL);
	return request->params_json;
}

/**
 * gowl_gabbrev_request_get_var:
 * @request: a request
 * @name: another variable of the same match (or a global one)
 *
 * Variables resolve in order, so this sees the ones before this one.
 *
 * Returns: (transfer none) (nullable): its value, %NULL when it has none
 */
const gchar *
gowl_gabbrev_request_get_var(
	GowlGabbrevRequest *request,
	const gchar        *name
){
	g_return_val_if_fail(request != NULL, NULL);
	g_return_val_if_fail(name != NULL, NULL);
	return g_hash_table_lookup(request->vars, name);
}

/**
 * gowl_gabbrev_request_get_vars:
 * @request: a request
 *
 * Returns: (transfer none) (element-type utf8 utf8): every value known
 *   so far, by variable name
 */
GHashTable *
gowl_gabbrev_request_get_vars(GowlGabbrevRequest *request)
{
	g_return_val_if_fail(request != NULL, NULL);
	return request->vars;
}

/**
 * gowl_gabbrev_request_get_trigger:
 * @request: a request
 *
 * Returns: (transfer none): what was typed to start the expansion, as
 *   typed (`;Today' for `;today' with propagate_case); empty when it
 *   was started from IPC or the search picker
 */
const gchar *
gowl_gabbrev_request_get_trigger(GowlGabbrevRequest *request)
{
	g_return_val_if_fail(request != NULL, NULL);
	return request->trigger;
}

/**
 * gowl_gabbrev_request_get_app_id:
 * @request: a request
 *
 * Returns: (transfer none): the app-id (X11: the class) of the window
 *   being typed into, empty when unknown
 */
const gchar *
gowl_gabbrev_request_get_app_id(GowlGabbrevRequest *request)
{
	g_return_val_if_fail(request != NULL, NULL);
	return request->app_id;
}

/**
 * gowl_gabbrev_request_get_title:
 * @request: a request
 *
 * Returns: (transfer none): the title of the window being typed into
 */
const gchar *
gowl_gabbrev_request_get_title(GowlGabbrevRequest *request)
{
	g_return_val_if_fail(request != NULL, NULL);
	return request->title;
}

/* The one place an answer is recorded.  Exactly one caller wins; the
   done notify runs outside the lock, on the answering thread. */
static gboolean
answer(
	GowlGabbrevRequest *request,
	const gchar        *value,
	const gchar        *error
){
	GowlGabbrevRequestDone done = NULL;
	gpointer data = NULL;

	gowl_fault_guard_hold();
	g_mutex_lock(&request->lock);
	if (request->answered || request->cancelled) {
		g_mutex_unlock(&request->lock);
		gowl_fault_guard_release();
		return FALSE;
	}
	request->answered = TRUE;
	if (error != NULL) {
		request->error = g_strdup(error);
	} else if (value != NULL && g_utf8_validate(value, -1, NULL)) {
		request->value = g_strdup(value);
	} else if (value != NULL) {
		/* Text the window cannot be given is an error, not mojibake */
		request->error = g_strdup("the value is not valid UTF-8");
	} else {
		request->value = g_strdup("");
	}
	done = request->done;
	data = request->done_data;
	g_mutex_unlock(&request->lock);
	gowl_fault_guard_release();

	pending_remove(request->id);
	if (done != NULL)
		done(request, data);
	return TRUE;
}

/**
 * gowl_gabbrev_request_return:
 * @request: a request
 * @value: (nullable): the value; %NULL is the empty string
 *
 * Answers @request, from any thread.  Text that is not UTF-8 is turned
 * into an error.
 *
 * Returns: %TRUE when this was the answer, %FALSE when the request had
 *   already been answered, timed out or been cancelled
 */
gboolean
gowl_gabbrev_request_return(
	GowlGabbrevRequest *request,
	const gchar        *value
){
	g_return_val_if_fail(request != NULL, FALSE);
	return answer(request, value != NULL ? value : "", NULL);
}

/**
 * gowl_gabbrev_request_return_error:
 * @request: a request
 * @message: why there is no value; shown in a notification
 *
 * Fails @request, from any thread.  The expansion it belongs to is
 * abandoned and the trigger the user typed is left where it is.
 *
 * Returns: %TRUE when this was the answer
 */
gboolean
gowl_gabbrev_request_return_error(
	GowlGabbrevRequest *request,
	const gchar        *message
){
	g_return_val_if_fail(request != NULL, FALSE);
	return answer(request, NULL,
	              message != NULL && *message != '\0' ? message : "failed");
}

/**
 * gowl_gabbrev_request_is_answered:
 * @request: a request
 *
 * A long provider can poll this to stop early once the expander gave
 * up on it.
 *
 * Returns: %TRUE once answered or cancelled
 */
gboolean
gowl_gabbrev_request_is_answered(GowlGabbrevRequest *request)
{
	gboolean answered;

	g_return_val_if_fail(request != NULL, TRUE);
	gowl_fault_guard_hold();
	g_mutex_lock(&request->lock);
	answered = request->answered || request->cancelled;
	g_mutex_unlock(&request->lock);
	gowl_fault_guard_release();
	return answered;
}

/**
 * gowl_gabbrev_request_return_by_id:
 * @id: a request's id (gowl_gabbrev_request_get_id())
 * @value: (nullable): the value, used when @error_message is %NULL
 * @error_message: (nullable): an error instead of a value
 *
 * Answers a request known only by its number, from any thread -- how
 * Lisp under cmacs, or a program at the end of the IPC socket
 * (`gabbrev-resolve'), answers.
 *
 * Returns: %TRUE when the request was waiting and this answered it
 */
gboolean
gowl_gabbrev_request_return_by_id(
	guint64      id,
	const gchar *value,
	const gchar *error_message
){
	GowlGabbrevRequest *r;
	gboolean ok;

	gowl_fault_guard_hold();
	g_mutex_lock(&pending_lock);
	r = pending != NULL ? g_hash_table_lookup(pending, &id) : NULL;
	if (r != NULL)
		gowl_gabbrev_request_ref(r);
	g_mutex_unlock(&pending_lock);
	gowl_fault_guard_release();
	if (r == NULL)
		return FALSE;

	ok = error_message != NULL
		? gowl_gabbrev_request_return_error(r, error_message)
		: gowl_gabbrev_request_return(r, value);
	gowl_gabbrev_request_unref(r);
	return ok;
}

/**
 * gowl_gabbrev_request_get_value: (skip)
 * @request: an answered request
 *
 * Returns: (transfer none) (nullable): the value, %NULL on an error
 */
const gchar *
gowl_gabbrev_request_get_value(GowlGabbrevRequest *request)
{
	const gchar *v;

	g_return_val_if_fail(request != NULL, NULL);
	g_mutex_lock(&request->lock);
	v = request->value;
	g_mutex_unlock(&request->lock);
	return v;
}

/**
 * gowl_gabbrev_request_get_error: (skip)
 * @request: an answered request
 *
 * Returns: (transfer none) (nullable): the error, %NULL with a value
 */
const gchar *
gowl_gabbrev_request_get_error(GowlGabbrevRequest *request)
{
	const gchar *e;

	g_return_val_if_fail(request != NULL, NULL);
	g_mutex_lock(&request->lock);
	e = request->error;
	g_mutex_unlock(&request->lock);
	return e;
}

/**
 * gowl_gabbrev_request_cancel: (skip)
 * @request: a request
 *
 * The expander no longer wants an answer: a late one is dropped and
 * the done notify is never called after this returns.
 */
void
gowl_gabbrev_request_cancel(GowlGabbrevRequest *request)
{
	g_return_if_fail(request != NULL);
	gowl_fault_guard_hold();
	g_mutex_lock(&request->lock);
	request->cancelled = TRUE;
	request->done = NULL;
	g_mutex_unlock(&request->lock);
	gowl_fault_guard_release();
	pending_remove(request->id);
}

/* ── The registries ──────────────────────────────────────────────────── */

struct _GowlGabbrevHandle {
	gint               ref_count;
	GowlGabbrevVarFunc var_func;
	GowlGabbrevFunc    func;
	gpointer           data;
	GDestroyNotify     destroy;
};

static GMutex      registry_lock;
static GHashTable *var_types;   /* gchar* -> GowlGabbrevHandle* */
static GHashTable *funcs;       /* gchar* -> GowlGabbrevHandle* */
static gint        generation;

static void
handle_unref(gpointer p)
{
	GowlGabbrevHandle *h = p;

	if (h == NULL || !g_atomic_int_dec_and_test(&h->ref_count))
		return;
	if (h->destroy != NULL && h->data != NULL)
		h->destroy(h->data);
	g_free(h);
}

static void
bump_generation(void)
{
	g_atomic_int_inc(&generation);
}

static void
registry_put(
	GHashTable       **table,
	const gchar       *name,
	GowlGabbrevHandle *h
){
	gowl_fault_guard_hold();
	g_mutex_lock(&registry_lock);
	if (*table == NULL)
		*table = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
		                               handle_unref);
	g_hash_table_replace(*table, g_strdup(name), h);
	g_mutex_unlock(&registry_lock);
	gowl_fault_guard_release();
	bump_generation();
}

static gboolean
registry_drop(
	GHashTable  *table,
	const gchar *name
){
	gboolean removed;

	gowl_fault_guard_hold();
	g_mutex_lock(&registry_lock);
	removed = table != NULL && g_hash_table_remove(table, name);
	g_mutex_unlock(&registry_lock);
	gowl_fault_guard_release();
	if (removed)
		bump_generation();
	return removed;
}

static GowlGabbrevHandle *
registry_get(
	GHashTable  *table,
	const gchar *name
){
	GowlGabbrevHandle *h;

	gowl_fault_guard_hold();
	g_mutex_lock(&registry_lock);
	h = table != NULL && name != NULL ? g_hash_table_lookup(table, name) : NULL;
	if (h != NULL)
		g_atomic_int_inc(&h->ref_count);
	g_mutex_unlock(&registry_lock);
	gowl_fault_guard_release();
	return h;
}

static GStrv
registry_names(GHashTable *table)
{
	GPtrArray *out;
	GHashTableIter iter;
	gpointer key;

	out = g_ptr_array_new();
	gowl_fault_guard_hold();
	g_mutex_lock(&registry_lock);
	if (table != NULL) {
		g_hash_table_iter_init(&iter, table);
		while (g_hash_table_iter_next(&iter, &key, NULL))
			g_ptr_array_add(out, g_strdup(key));
	}
	g_mutex_unlock(&registry_lock);
	gowl_fault_guard_release();
	g_ptr_array_sort_values(out, (GCompareFunc)g_strcmp0);
	g_ptr_array_add(out, NULL);
	return (GStrv)g_ptr_array_free(out, FALSE);
}

/**
 * gowl_gabbrev_register_var_type:
 * @type: the name YAML uses in `type:'
 * @func: (scope notified) (closure user_data) (destroy destroy): resolves
 *   one variable of the type
 * @user_data: (nullable): passed to @func
 * @destroy: (nullable): frees @user_data once nothing is calling @func
 *
 * Adds a variable type, or replaces one -- a registered type wins over
 * the module's built-in type of the same name, which is how an embedder
 * gives `choice' and `form' its own picker.  Can be called before the
 * module loads, and from any thread.
 */
void
gowl_gabbrev_register_var_type(
	const gchar        *type,
	GowlGabbrevVarFunc  func,
	gpointer            user_data,
	GDestroyNotify      destroy
){
	GowlGabbrevHandle *h;

	g_return_if_fail(type != NULL && *type != '\0');
	g_return_if_fail(func != NULL);

	h = g_new0(GowlGabbrevHandle, 1);
	h->ref_count = 1;
	h->var_func = func;
	h->data = user_data;
	h->destroy = destroy;
	registry_put(&var_types, type, h);
}

/**
 * gowl_gabbrev_unregister_var_type:
 * @type: a registered type
 *
 * Removes a registered type; a built-in type of the same name is used
 * again.
 *
 * Returns: %TRUE when one was removed
 */
gboolean
gowl_gabbrev_unregister_var_type(const gchar *type)
{
	g_return_val_if_fail(type != NULL, FALSE);
	return registry_drop(var_types, type);
}

/**
 * gowl_gabbrev_list_var_types:
 *
 * Returns: (transfer full): every registered type name, sorted (the
 *   module's built-in types are not in it)
 */
GStrv
gowl_gabbrev_list_var_types(void)
{
	return registry_names(var_types);
}

/**
 * gowl_gabbrev_register_func:
 * @name: the name a `c' variable gives in `params: {func: NAME}'
 * @func: (scope notified) (closure user_data) (destroy destroy): computes
 *   the value
 * @user_data: (nullable): passed to @func
 * @destroy: (nullable): frees @user_data
 *
 * Names a C function for YAML to call, from a C config, a module or an
 * embedder.  It runs on the compositor thread under the fault guard.
 */
void
gowl_gabbrev_register_func(
	const gchar     *name,
	GowlGabbrevFunc  func,
	gpointer         user_data,
	GDestroyNotify   destroy
){
	GowlGabbrevHandle *h;

	g_return_if_fail(name != NULL && *name != '\0');
	g_return_if_fail(func != NULL);

	h = g_new0(GowlGabbrevHandle, 1);
	h->ref_count = 1;
	h->func = func;
	h->data = user_data;
	h->destroy = destroy;
	registry_put(&funcs, name, h);
}

/**
 * gowl_gabbrev_unregister_func:
 * @name: a registered name
 *
 * Returns: %TRUE when one was removed
 */
gboolean
gowl_gabbrev_unregister_func(const gchar *name)
{
	g_return_val_if_fail(name != NULL, FALSE);
	return registry_drop(funcs, name);
}

/**
 * gowl_gabbrev_list_funcs:
 *
 * Returns: (transfer full): every registered function name, sorted
 */
GStrv
gowl_gabbrev_list_funcs(void)
{
	return registry_names(funcs);
}

/**
 * gowl_gabbrev_lookup_var_type: (skip)
 * @type: a type name
 *
 * Returns: (transfer full) (nullable): the registered type, held until
 *   gowl_gabbrev_handle_release()
 */
GowlGabbrevHandle *
gowl_gabbrev_lookup_var_type(const gchar *type)
{
	return registry_get(var_types, type);
}

/**
 * gowl_gabbrev_lookup_func: (skip)
 * @name: a function name
 *
 * Returns: (transfer full) (nullable): the registered function
 */
GowlGabbrevHandle *
gowl_gabbrev_lookup_func(const gchar *name)
{
	return registry_get(funcs, name);
}

/**
 * gowl_gabbrev_handle_call_var: (skip)
 * @handle: a variable type
 * @request: what to resolve
 */
void
gowl_gabbrev_handle_call_var(
	GowlGabbrevHandle  *handle,
	GowlGabbrevRequest *request
){
	g_return_if_fail(handle != NULL && handle->var_func != NULL);
	handle->var_func(request, handle->data);
}

/**
 * gowl_gabbrev_handle_call_func: (skip)
 * @handle: a function
 * @request: what to resolve
 *
 * Returns: (transfer full) (nullable): what the function returned
 */
gchar *
gowl_gabbrev_handle_call_func(
	GowlGabbrevHandle  *handle,
	GowlGabbrevRequest *request
){
	g_return_val_if_fail(handle != NULL && handle->func != NULL, NULL);
	return handle->func(request, handle->data);
}

/**
 * gowl_gabbrev_handle_release: (skip)
 * @handle: (transfer full): from a lookup
 */
void
gowl_gabbrev_handle_release(GowlGabbrevHandle *handle)
{
	handle_unref(handle);
}

/* ── Sources ─────────────────────────────────────────────────────────── */

static GMutex     sources_lock;
static GPtrArray *sources;      /* GowlGabbrevSource* */

/**
 * gowl_gabbrev_source_free: (skip)
 * @source: (transfer full): a source
 */
void
gowl_gabbrev_source_free(GowlGabbrevSource *source)
{
	if (source == NULL)
		return;
	g_free(source->text);
	g_free(source->id);
	g_free(source);
}

/* Adds a source; one with an @id replaces the source of the same id
 * where it stood, so a re-added set keeps its place in the load order
 * (later sources win between equal triggers). */
static void
add_source(
	gboolean     is_file,
	const gchar *text,
	const gchar *id
){
	GowlGabbrevSource *s;
	gboolean replaced = FALSE;
	guint i;

	s = g_new0(GowlGabbrevSource, 1);
	s->is_file = is_file;
	s->text = g_strdup(text);
	s->id = g_strdup(id);
	gowl_fault_guard_hold();
	g_mutex_lock(&sources_lock);
	if (sources == NULL)
		sources = g_ptr_array_new_with_free_func(
			(GDestroyNotify)gowl_gabbrev_source_free);
	for (i = 0; id != NULL && i < sources->len; i++) {
		GowlGabbrevSource *old = g_ptr_array_index(sources, i);

		if (g_strcmp0(old->id, id) == 0) {
			gowl_gabbrev_source_free(old);
			g_ptr_array_index(sources, i) = s;
			replaced = TRUE;
			break;
		}
	}
	if (!replaced)
		g_ptr_array_add(sources, s);
	g_mutex_unlock(&sources_lock);
	gowl_fault_guard_release();
	bump_generation();
}

/**
 * gowl_gabbrev_add_file:
 * @path: a match file, a directory of them, or an espanso configuration
 *   directory (one holding `config/' and `match/'); `~/' is expanded when
 *   it is read
 *
 * Loads @path in addition to everything else, for the rest of the
 * process: it survives a config reload.  The C config's way of saying
 * "and my espanso config too":
 *
 * |[<!-- language="C" -->
 * gowl_gabbrev_add_file("~/.config/espanso");
 * ]|
 *
 * A path that does not exist is reported when it is read, not here, so
 * a directory created later still loads on `gabbrev-reload'.
 */
void
gowl_gabbrev_add_file(const gchar *path)
{
	g_return_if_fail(path != NULL && *path != '\0');
	add_source(TRUE, path, NULL);
}

/**
 * gowl_gabbrev_add_yaml:
 * @yaml: a match file's text: `matches:', `global_vars:', `imports:'
 *   and the options, exactly as a file would hold them
 * @error: return location for a parse error
 *
 * Adds matches written in the program itself, for the rest of the
 * process.  The text is parsed here, so a syntax error is reported to
 * the caller; what it means (unknown keys, bad variables) is checked
 * when the module loads it, like any file.
 *
 * Returns: %TRUE when the text parsed and was added
 */
gboolean
gowl_gabbrev_add_yaml(
	const gchar  *yaml,
	GError      **error
){
	g_autoptr(YamlParser) parser = NULL;
	GError *local = NULL;

	g_return_val_if_fail(yaml != NULL, FALSE);

	/* Always with an error location: yaml-glib dereferences a NULL one
	   on malformed input */
	parser = yaml_parser_new();
	if (!yaml_parser_load_from_data(parser, yaml, -1, &local)) {
		g_propagate_error(error, local);
		return FALSE;
	}
	add_source(FALSE, yaml, NULL);
	return TRUE;
}

/**
 * gowl_gabbrev_add_yaml_source:
 * @id: a name for the source, such as the module that owns it
 * @yaml: a match file's text, as for gowl_gabbrev_add_yaml()
 * @error: return location for a parse error
 *
 * Like gowl_gabbrev_add_yaml(), but the source has a name: adding again
 * under the same @id REPLACES it (in its place in the load order), and
 * gowl_gabbrev_remove_source() takes it out again without touching any
 * other source.  This is how a module that owns some matches -- the
 * hotkey module's hotstrings -- reloads them without disturbing the
 * user's own:
 *
 * |[<!-- language="C" -->
 * gowl_gabbrev_add_yaml_source("hotkey",
 *     "matches:\n  - {trigger: btw, replace: by the way, word: true}\n",
 *     &error);
 * ]|
 *
 * Returns: %TRUE when the text parsed and was added
 */
gboolean
gowl_gabbrev_add_yaml_source(
	const gchar  *id,
	const gchar  *yaml,
	GError      **error
){
	g_autoptr(YamlParser) parser = NULL;
	GError *local = NULL;

	g_return_val_if_fail(id != NULL && *id != '\0', FALSE);
	g_return_val_if_fail(yaml != NULL, FALSE);

	/* Always with an error location: yaml-glib dereferences a NULL one
	   on malformed input */
	parser = yaml_parser_new();
	if (!yaml_parser_load_from_data(parser, yaml, -1, &local)) {
		g_propagate_error(error, local);
		return FALSE;
	}
	add_source(FALSE, yaml, id);
	return TRUE;
}

/**
 * gowl_gabbrev_remove_source:
 * @id: a name given to gowl_gabbrev_add_yaml_source()
 *
 * Takes that source out; every other source stays.
 *
 * Returns: %TRUE when there was one
 */
gboolean
gowl_gabbrev_remove_source(const gchar *id)
{
	gboolean found = FALSE;
	guint i;

	g_return_val_if_fail(id != NULL, FALSE);
	gowl_fault_guard_hold();
	g_mutex_lock(&sources_lock);
	for (i = 0; sources != NULL && i < sources->len; i++) {
		GowlGabbrevSource *s = g_ptr_array_index(sources, i);

		if (g_strcmp0(s->id, id) == 0) {
			g_ptr_array_remove_index(sources, i);
			found = TRUE;
			break;
		}
	}
	g_mutex_unlock(&sources_lock);
	gowl_fault_guard_release();
	if (found)
		bump_generation();
	return found;
}

/**
 * gowl_gabbrev_clear_sources:
 *
 * Forgets every file and text added with gowl_gabbrev_add_file() and
 * gowl_gabbrev_add_yaml().
 */
void
gowl_gabbrev_clear_sources(void)
{
	gowl_fault_guard_hold();
	g_mutex_lock(&sources_lock);
	if (sources != NULL)
		g_ptr_array_set_size(sources, 0);
	g_mutex_unlock(&sources_lock);
	gowl_fault_guard_release();
	bump_generation();
}

/**
 * gowl_gabbrev_dup_sources: (skip)
 *
 * Returns: (transfer full) (element-type GowlGabbrevSource): a copy of
 *   the sources, in the order they were added
 */
GPtrArray *
gowl_gabbrev_dup_sources(void)
{
	GPtrArray *out;
	guint i;

	out = g_ptr_array_new_with_free_func(
		(GDestroyNotify)gowl_gabbrev_source_free);
	gowl_fault_guard_hold();
	g_mutex_lock(&sources_lock);
	for (i = 0; sources != NULL && i < sources->len; i++) {
		GowlGabbrevSource *s = g_ptr_array_index(sources, i);
		GowlGabbrevSource *c = g_new0(GowlGabbrevSource, 1);

		c->is_file = s->is_file;
		c->text = g_strdup(s->text);
		c->id = g_strdup(s->id);
		g_ptr_array_add(out, c);
	}
	g_mutex_unlock(&sources_lock);
	gowl_fault_guard_release();
	return out;
}

/**
 * gowl_gabbrev_get_generation: (skip)
 *
 * Returns: a number that changes whenever a source, a type or a
 *   function is added or removed
 */
guint
gowl_gabbrev_get_generation(void)
{
	return (guint)g_atomic_int_get(&generation);
}
