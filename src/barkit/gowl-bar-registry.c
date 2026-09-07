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

#include "barkit/gowl-bar-registry.h"
#include "barkit/gowl-bar-guard.h"

#include <gio/gio.h>
#include <gmodule.h>
#include <string.h>

#include <crispy.h>

/**
 * SECTION:gowl-bar-registry
 * @title: GowlBarRegistry
 * @short_description: what the bar can build, and what it refuses to
 *
 * The registry answers two questions: given a name from a config, what
 * plugin does that build; and given a name that has hurt this session
 * before, should it be built at all.
 *
 * The second question is the reason the registry keeps a journal on
 * disk.  Bar plugins run inside the compositor, so a plugin that
 * crashes on load crashes the session --- and without a record of what
 * was being loaded at the time, the next start loads it again and
 * crashes again.  The journal turns that loop into one bad session
 * followed by a notification naming the culprit.
 */

/* The exported symbol a loadable plugin file must provide. */
#define GOWL_BAR_PLUGIN_QUERY_SYMBOL "gowl_bar_plugin_query"

/* Journal group names. */
#define JOURNAL_GROUP_LOADING    "loading"
#define JOURNAL_GROUP_QUARANTINE "quarantine"

typedef struct {
	gchar                *name;
	gchar                *title;
	gchar                *description;
	gchar                *version;

	GType                 type;          /* 0 unless a GType entry */
	GowlBarPluginFactory  factory;
	gpointer              factory_data;
	GDestroyNotify        factory_destroy;

	const GowlBarPluginVTable *vtable;    /* borrowed from the module */

	gchar                *source_path;    /* the .so/.c on disk */
	GModule              *module;         /* NULL for built-ins */
	gboolean              builtin;
} RegistryEntry;

struct _GowlBarRegistry {
	GObject parent_instance;

	GHashTable *entries;    /* gchar* -> RegistryEntry* */
	GHashTable *aliases;    /* gchar* -> gchar* */
	GHashTable *quarantine; /* gchar* -> gchar* (reason) */

	gchar      *state_dir;
	gchar      *journal_path;

	/* The crispy toolchain, created lazily: a session that never
	   loads a .c plugin should not pay for probing gcc. */
	CrispyGccCompiler *compiler;
	CrispyFileCache   *cache;
	gboolean           compiler_failed;
};

G_DEFINE_FINAL_TYPE(GowlBarRegistry, gowl_bar_registry, G_TYPE_OBJECT)

enum {
	SIGNAL_PLUGIN_REGISTERED,
	SIGNAL_PLUGIN_UNLOADED,
	SIGNAL_PLUGIN_FAILED,
	SIGNAL_LAST
};

static guint registry_signals[SIGNAL_LAST];

/* ----------------------------------------------------------------
 * Entry lifetime
 * ---------------------------------------------------------------- */

static void
registry_entry_free(gpointer data)
{
	RegistryEntry *entry = data;

	if (entry == NULL)
		return;

	if (entry->factory_destroy != NULL && entry->factory_data != NULL)
		entry->factory_destroy(entry->factory_data);

	g_free(entry->name);
	g_free(entry->title);
	g_free(entry->description);
	g_free(entry->version);
	g_free(entry->source_path);
	/* entry->module is deliberately not closed; see
	   gowl_bar_registry_unload(). */
	g_free(entry);
}

/* ----------------------------------------------------------------
 * GObject
 * ---------------------------------------------------------------- */

static void
gowl_bar_registry_finalize(GObject *object)
{
	GowlBarRegistry *self = GOWL_BAR_REGISTRY(object);

	g_clear_pointer(&self->entries, g_hash_table_unref);
	g_clear_pointer(&self->aliases, g_hash_table_unref);
	g_clear_pointer(&self->quarantine, g_hash_table_unref);
	g_clear_object(&self->compiler);
	g_clear_object(&self->cache);
	g_free(self->state_dir);
	g_free(self->journal_path);

	G_OBJECT_CLASS(gowl_bar_registry_parent_class)->finalize(object);
}

static void
gowl_bar_registry_class_init(GowlBarRegistryClass *klass)
{
	G_OBJECT_CLASS(klass)->finalize = gowl_bar_registry_finalize;

	/**
	 * GowlBarRegistry::plugin-registered:
	 * @self: the registry
	 * @name: the plugin's registry name
	 *
	 * Emitted once per plugin a load published.
	 */
	registry_signals[SIGNAL_PLUGIN_REGISTERED] =
		g_signal_new("plugin-registered", G_TYPE_FROM_CLASS(klass),
		             G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
		             G_TYPE_NONE, 1, G_TYPE_STRING);

	/**
	 * GowlBarRegistry::plugin-unloaded:
	 * @self: the registry
	 * @name: the plugin's registry name
	 *
	 * Emitted when a plugin leaves the registry.  The host must drop
	 * every instance of it before returning from this handler ---
	 * after it, the vtable those instances point at may be gone.
	 */
	registry_signals[SIGNAL_PLUGIN_UNLOADED] =
		g_signal_new("plugin-unloaded", G_TYPE_FROM_CLASS(klass),
		             G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
		             G_TYPE_NONE, 1, G_TYPE_STRING);

	/**
	 * GowlBarRegistry::plugin-failed:
	 * @self: the registry
	 * @name: the plugin or file that failed
	 * @message: what went wrong
	 *
	 * Emitted on a failed load or a quarantine, so the host can raise
	 * a toast rather than leaving the failure in a log nobody reads.
	 */
	registry_signals[SIGNAL_PLUGIN_FAILED] =
		g_signal_new("plugin-failed", G_TYPE_FROM_CLASS(klass),
		             G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
		             G_TYPE_NONE, 2, G_TYPE_STRING, G_TYPE_STRING);
}

static void
gowl_bar_registry_init(GowlBarRegistry *self)
{
	self->entries = g_hash_table_new_full(g_str_hash, g_str_equal,
	                                      g_free, registry_entry_free);
	self->aliases = g_hash_table_new_full(g_str_hash, g_str_equal,
	                                      g_free, g_free);
	self->quarantine = g_hash_table_new_full(g_str_hash, g_str_equal,
	                                         g_free, g_free);
}

/**
 * gowl_bar_registry_new:
 * @state_dir: (nullable): a writable directory for the load journal
 *
 * Returns: (transfer full): a new registry
 */
GowlBarRegistry *
gowl_bar_registry_new(const gchar *state_dir)
{
	GowlBarRegistry *self;

	self = (GowlBarRegistry *)g_object_new(GOWL_TYPE_BAR_REGISTRY, NULL);

	if (state_dir != NULL) {
		self->state_dir = g_strdup(state_dir);
	} else {
		self->state_dir = g_build_filename(g_get_user_state_dir(),
		                                   "gowl", NULL);
	}
	g_mkdir_with_parents(self->state_dir, 0700);
	self->journal_path = g_build_filename(self->state_dir,
	                                      "bar-plugins.journal", NULL);

	return self;
}

/* ----------------------------------------------------------------
 * Journal
 * ---------------------------------------------------------------- */

/* Load the journal, or an empty one when it does not exist yet. */
static GKeyFile *
journal_load(GowlBarRegistry *self)
{
	GKeyFile *kf;

	kf = g_key_file_new();
	if (self->journal_path != NULL) {
		g_key_file_load_from_file(kf, self->journal_path,
		                          G_KEY_FILE_NONE, NULL);
	}
	return kf;
}

static void
journal_save(GowlBarRegistry *self, GKeyFile *kf)
{
	g_autoptr(GError) error = NULL;

	if (self->journal_path == NULL)
		return;
	if (!g_key_file_save_to_file(kf, self->journal_path, &error)) {
		g_warning("gowl-bar: could not write the plugin journal "
		          "at %s: %s -- a plugin that crashes the session "
		          "will not be caught next start",
		          self->journal_path, error->message);
	}
}

/* Record that a load is starting.  The file is fsync'd by
   g_key_file_save_to_file's rename, which is what makes this survive
   the session dying mid-load. */
static void
journal_begin(GowlBarRegistry *self, const gchar *name, const gchar *path)
{
	g_autoptr(GKeyFile) kf = NULL;

	kf = journal_load(self);
	g_key_file_set_string(kf, JOURNAL_GROUP_LOADING, name,
	                      (path != NULL) ? path : "");
	journal_save(self, kf);
}

static void
journal_end(GowlBarRegistry *self, const gchar *name)
{
	g_autoptr(GKeyFile) kf = NULL;

	kf = journal_load(self);
	if (g_key_file_has_group(kf, JOURNAL_GROUP_LOADING)) {
		g_key_file_remove_key(kf, JOURNAL_GROUP_LOADING, name, NULL);
		journal_save(self, kf);
	}
}

static void
journal_quarantine(GowlBarRegistry *self, const gchar *name,
                   const gchar *reason)
{
	g_autoptr(GKeyFile) kf = NULL;

	kf = journal_load(self);
	g_key_file_set_string(kf, JOURNAL_GROUP_QUARANTINE, name,
	                      (reason != NULL) ? reason : "unspecified");
	g_key_file_remove_key(kf, JOURNAL_GROUP_LOADING, name, NULL);
	journal_save(self, kf);
}

/**
 * gowl_bar_registry_recover_journal:
 * @self: a registry
 *
 * Returns: (transfer full) (nullable): the quarantined name, or %NULL
 */
gchar *
gowl_bar_registry_recover_journal(GowlBarRegistry *self)
{
	g_autoptr(GKeyFile) kf = NULL;
	g_auto(GStrv) loading = NULL;
	g_auto(GStrv) held = NULL;
	gchar *culprit;
	gsize n, i;

	g_return_val_if_fail(GOWL_IS_BAR_REGISTRY(self), NULL);

	kf = journal_load(self);

	/* Re-arm the quarantine set from the last session first, so the
	   caller's loads are refused before they run. */
	held = g_key_file_get_keys(kf, JOURNAL_GROUP_QUARANTINE, &n, NULL);
	if (held != NULL) {
		for (i = 0; i < n; i++) {
			g_autofree gchar *reason = NULL;

			reason = g_key_file_get_string(kf,
				JOURNAL_GROUP_QUARANTINE, held[i], NULL);
			g_hash_table_insert(self->quarantine,
				g_strdup(held[i]),
				g_strdup((reason != NULL) ? reason
				                          : "quarantined"));
		}
	}

	loading = g_key_file_get_keys(kf, JOURNAL_GROUP_LOADING, &n, NULL);
	if (loading == NULL || n == 0)
		return NULL;

	/* Exactly one load is ever in flight, so the first key is the
	   one that never finished. */
	culprit = g_strdup(loading[0]);

	g_hash_table_insert(self->quarantine, g_strdup(culprit),
		g_strdup("the session did not survive loading it last time"));
	journal_quarantine(self, culprit,
		"the session did not survive loading it last time");

	g_warning("gowl-bar: '%s' was being loaded when the last session "
	          "ended; holding it back", culprit);
	g_signal_emit(self, registry_signals[SIGNAL_PLUGIN_FAILED], 0,
	              culprit,
	              "This plugin was loading when the last session ended. "
	              "It is being held back until you clear it.");
	return culprit;
}

/**
 * gowl_bar_registry_quarantine:
 * @self: a registry
 * @name: the plugin to hold back
 * @reason: why
 */
void
gowl_bar_registry_quarantine(GowlBarRegistry *self, const gchar *name,
                             const gchar *reason)
{
	g_return_if_fail(GOWL_IS_BAR_REGISTRY(self));
	g_return_if_fail(name != NULL);

	g_hash_table_insert(self->quarantine, g_strdup(name),
	                    g_strdup((reason != NULL) ? reason
	                                              : "quarantined"));
	journal_quarantine(self, name, reason);

	g_signal_emit(self, registry_signals[SIGNAL_PLUGIN_FAILED], 0,
	              name, (reason != NULL) ? reason : "quarantined");
}

/**
 * gowl_bar_registry_is_quarantined:
 * @self: a registry
 * @name: a plugin name
 *
 * Returns: %TRUE when @name is being held back
 */
gboolean
gowl_bar_registry_is_quarantined(GowlBarRegistry *self, const gchar *name)
{
	g_return_val_if_fail(GOWL_IS_BAR_REGISTRY(self), FALSE);

	if (name == NULL)
		return FALSE;
	return g_hash_table_contains(self->quarantine, name);
}

/**
 * gowl_bar_registry_quarantine_reason:
 * @self: a registry
 * @name: a plugin name
 *
 * Returns: (transfer none) (nullable): why @name is held back
 */
const gchar *
gowl_bar_registry_quarantine_reason(GowlBarRegistry *self, const gchar *name)
{
	g_return_val_if_fail(GOWL_IS_BAR_REGISTRY(self), NULL);

	if (name == NULL)
		return NULL;
	return (const gchar *)g_hash_table_lookup(self->quarantine, name);
}

/**
 * gowl_bar_registry_clear_quarantine:
 * @self: a registry
 * @name: a plugin name
 *
 * Returns: %TRUE when @name was held back and now is not
 */
gboolean
gowl_bar_registry_clear_quarantine(GowlBarRegistry *self, const gchar *name)
{
	g_autoptr(GKeyFile) kf = NULL;

	g_return_val_if_fail(GOWL_IS_BAR_REGISTRY(self), FALSE);
	g_return_val_if_fail(name != NULL, FALSE);

	if (!g_hash_table_remove(self->quarantine, name))
		return FALSE;

	kf = journal_load(self);
	g_key_file_remove_key(kf, JOURNAL_GROUP_QUARANTINE, name, NULL);
	journal_save(self, kf);
	return TRUE;
}

/**
 * gowl_bar_registry_list_quarantined:
 * @self: a registry
 *
 * Returns: (transfer full) (array zero-terminated=1): the held names
 */
gchar **
gowl_bar_registry_list_quarantined(GowlBarRegistry *self)
{
	GHashTableIter iter;
	gpointer key;
	GPtrArray *names;

	g_return_val_if_fail(GOWL_IS_BAR_REGISTRY(self), NULL);

	names = g_ptr_array_new();
	g_hash_table_iter_init(&iter, self->quarantine);
	while (g_hash_table_iter_next(&iter, &key, NULL))
		g_ptr_array_add(names, g_strdup((const gchar *)key));
	g_ptr_array_add(names, NULL);
	return (gchar **)g_ptr_array_free(names, FALSE);
}

/* ----------------------------------------------------------------
 * Registration
 * ---------------------------------------------------------------- */

static RegistryEntry *
registry_entry_new(const gchar *name, const gchar *title,
                   const gchar *description)
{
	RegistryEntry *entry;

	entry = g_new0(RegistryEntry, 1);
	entry->name        = g_strdup(name);
	entry->title       = g_strdup(title);
	entry->description = g_strdup(description);
	return entry;
}

static void
registry_insert(GowlBarRegistry *self, RegistryEntry *entry)
{
	g_hash_table_insert(self->entries, g_strdup(entry->name), entry);
	g_signal_emit(self, registry_signals[SIGNAL_PLUGIN_REGISTERED], 0,
	              entry->name);
}

/**
 * gowl_bar_registry_register_type:
 * @self: a registry
 * @name: the registry name
 * @type: a #GowlBarPlugin subtype
 * @title: (nullable): a human-readable name
 * @description: (nullable): one line describing the plugin
 */
void
gowl_bar_registry_register_type(GowlBarRegistry *self, const gchar *name,
                                GType type, const gchar *title,
                                const gchar *description)
{
	RegistryEntry *entry;

	g_return_if_fail(GOWL_IS_BAR_REGISTRY(self));
	g_return_if_fail(name != NULL);
	g_return_if_fail(g_type_is_a(type, GOWL_TYPE_BAR_PLUGIN));

	entry = registry_entry_new(name, title, description);
	entry->type    = type;
	entry->builtin = TRUE;
	registry_insert(self, entry);
}

/**
 * gowl_bar_registry_register_factory:
 * @self: a registry
 * @name: the registry name
 * @factory: builds instances
 * @user_data: passed to @factory
 * @destroy: (nullable): frees @user_data
 * @title: (nullable): a human-readable name
 * @description: (nullable): one line describing the plugin
 */
void
gowl_bar_registry_register_factory(GowlBarRegistry *self, const gchar *name,
                                   GowlBarPluginFactory factory,
                                   gpointer user_data,
                                   GDestroyNotify destroy,
                                   const gchar *title,
                                   const gchar *description)
{
	RegistryEntry *entry;

	g_return_if_fail(GOWL_IS_BAR_REGISTRY(self));
	g_return_if_fail(name != NULL);
	g_return_if_fail(factory != NULL);

	entry = registry_entry_new(name, title, description);
	entry->factory         = factory;
	entry->factory_data    = user_data;
	entry->factory_destroy = destroy;
	entry->builtin         = TRUE;
	registry_insert(self, entry);
}

/**
 * gowl_bar_registry_register_vtable:
 * @self: a registry
 * @name: the registry name
 * @title: (nullable): a human-readable name
 * @description: (nullable): one line describing the plugin
 * @vtable: the callbacks
 */
void
gowl_bar_registry_register_vtable(GowlBarRegistry *self, const gchar *name,
                                  const gchar *title,
                                  const gchar *description,
                                  const GowlBarPluginVTable *vtable)
{
	RegistryEntry *entry;

	g_return_if_fail(GOWL_IS_BAR_REGISTRY(self));
	g_return_if_fail(name != NULL);
	g_return_if_fail(vtable != NULL);

	entry = registry_entry_new(name, title, description);
	entry->vtable  = vtable;
	entry->builtin = TRUE;
	registry_insert(self, entry);
}

/**
 * gowl_bar_registry_register_alias:
 * @self: a registry
 * @alias: the alternative name
 * @target: the name it resolves to
 */
void
gowl_bar_registry_register_alias(GowlBarRegistry *self, const gchar *alias,
                                 const gchar *target)
{
	g_return_if_fail(GOWL_IS_BAR_REGISTRY(self));
	g_return_if_fail(alias != NULL);
	g_return_if_fail(target != NULL);

	g_hash_table_insert(self->aliases, g_strdup(alias), g_strdup(target));
}

/* Resolve an alias chain to a real entry name.  Bounded so a pair of
   aliases pointing at each other cannot spin. */
static const gchar *
registry_resolve(GowlBarRegistry *self, const gchar *name)
{
	const gchar *current;
	gint hops;

	current = name;
	for (hops = 0; hops < 8; hops++) {
		const gchar *next;

		if (g_hash_table_contains(self->entries, current))
			return current;
		next = (const gchar *)g_hash_table_lookup(self->aliases,
		                                          current);
		if (next == NULL)
			return current;
		current = next;
	}
	return current;
}

/**
 * gowl_bar_registry_has:
 * @self: a registry
 * @name: a name or alias
 *
 * Returns: %TRUE when @name resolves to a registered plugin
 */
gboolean
gowl_bar_registry_has(GowlBarRegistry *self, const gchar *name)
{
	g_return_val_if_fail(GOWL_IS_BAR_REGISTRY(self), FALSE);

	if (name == NULL)
		return FALSE;
	return g_hash_table_contains(self->entries,
	                             registry_resolve(self, name));
}

/* g_ptr_array_sort hands the comparator pointers TO the elements, so
   the strings need one more dereference than g_strcmp0 expects. */
static gint
registry_cmp_names(gconstpointer a, gconstpointer b)
{
	return g_strcmp0(*(const gchar * const *)a,
	                 *(const gchar * const *)b);
}

/**
 * gowl_bar_registry_list:
 * @self: a registry
 *
 * Returns: (transfer full) (array zero-terminated=1): the names
 */
gchar **
gowl_bar_registry_list(GowlBarRegistry *self)
{
	GHashTableIter iter;
	gpointer key;
	GPtrArray *names;

	g_return_val_if_fail(GOWL_IS_BAR_REGISTRY(self), NULL);

	names = g_ptr_array_new();
	g_hash_table_iter_init(&iter, self->entries);
	while (g_hash_table_iter_next(&iter, &key, NULL))
		g_ptr_array_add(names, g_strdup((const gchar *)key));
	g_ptr_array_sort(names, registry_cmp_names);
	g_ptr_array_add(names, NULL);
	return (gchar **)g_ptr_array_free(names, FALSE);
}

/**
 * gowl_bar_registry_describe:
 * @self: a registry
 * @name: a registered name
 *
 * Returns: (transfer full) (nullable): a one-line description
 */
gchar *
gowl_bar_registry_describe(GowlBarRegistry *self, const gchar *name)
{
	RegistryEntry *entry;

	g_return_val_if_fail(GOWL_IS_BAR_REGISTRY(self), NULL);

	if (name == NULL)
		return NULL;
	entry = g_hash_table_lookup(self->entries,
	                            registry_resolve(self, name));
	if (entry == NULL)
		return NULL;

	return g_strdup_printf("%-16s %-10s %-8s %s%s%s",
		entry->name,
		entry->builtin ? "builtin" : "loaded",
		(entry->version != NULL) ? entry->version : "-",
		(entry->description != NULL) ? entry->description : "",
		gowl_bar_registry_is_quarantined(self, entry->name)
			? "  [QUARANTINED: " : "",
		gowl_bar_registry_is_quarantined(self, entry->name)
			? gowl_bar_registry_quarantine_reason(self,
			                                      entry->name)
			: "");
}

/* ----------------------------------------------------------------
 * Instantiation
 * ---------------------------------------------------------------- */

/**
 * gowl_bar_registry_instantiate:
 * @self: a registry
 * @spec: a widget spec
 * @error: (nullable): return location for a #GError
 *
 * Returns: (transfer full) (nullable): a new plugin
 */
GowlBarPlugin *
gowl_bar_registry_instantiate(GowlBarRegistry *self, const gchar *spec,
                              GError **error)
{
	g_autofree gchar *work = NULL;
	const gchar *resolved;
	RegistryEntry *entry;
	GowlBarPlugin *plugin;
	gchar *param;
	gchar *at;
	gint interval;

	g_return_val_if_fail(GOWL_IS_BAR_REGISTRY(self), NULL);
	g_return_val_if_fail(spec != NULL, NULL);

	work = g_strdup(spec);

	/* A trailing "@N" is a per-instance poll interval.  The LAST '@'
	   wins so a command parameter that contains one -- `cmd:ssh
	   me@host@5' -- still parses the way the user meant. */
	interval = -1;
	at = strrchr(work, '@');
	if (at != NULL && at[1] != '\0') {
		gchar *end = NULL;
		gint64 n;

		n = g_ascii_strtoll(at + 1, &end, 10);
		if (n > 0 && end != NULL && *end == '\0') {
			*at = '\0';
			interval = (gint)n;
		}
	}

	param = strchr(work, ':');
	if (param != NULL) {
		*param = '\0';
		param++;
		if (param[0] == '\0')
			param = NULL;
	}

	resolved = registry_resolve(self, work);

	if (gowl_bar_registry_is_quarantined(self, resolved)) {
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
		            "plugin '%s' is quarantined: %s", resolved,
		            gowl_bar_registry_quarantine_reason(self,
		                                                resolved));
		return NULL;
	}

	entry = g_hash_table_lookup(self->entries, resolved);
	if (entry == NULL) {
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
		            "no bar plugin named '%s'", work);
		return NULL;
	}

	if (entry->vtable != NULL) {
		plugin = gowl_bar_plugin_proxy_new(spec, entry->title,
		                                   entry->description,
		                                   entry->vtable);
	} else if (entry->factory != NULL) {
		plugin = entry->factory(entry->name, param,
		                        entry->factory_data);
		if (plugin != NULL)
			gowl_bar_plugin_set_id(plugin, spec);
	} else if (entry->type != 0) {
		plugin = (GowlBarPlugin *)g_object_new(entry->type,
		                                       "id", spec, NULL);
	} else {
		plugin = NULL;
	}

	if (plugin == NULL) {
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
		            "bar plugin '%s' could not be instantiated",
		            resolved);
		return NULL;
	}

	/* The pieces of the spec become settings, so a plugin reads its
	   parameter the same way it reads anything else the user set. */
	gowl_bar_plugin_set_setting(plugin, "name", entry->name);
	if (param != NULL)
		gowl_bar_plugin_set_setting(plugin, "param", param);
	if (interval > 0) {
		g_autofree gchar *ival = NULL;

		ival = g_strdup_printf("%d", interval);
		gowl_bar_plugin_set_setting(plugin, "interval", ival);
	}

	return plugin;
}

/* ----------------------------------------------------------------
 * Loading
 * ---------------------------------------------------------------- */

/* Include flags for compiling a .c plugin.
 *
 * The in-tree headers come first and an installed gowl second, which
 * is the opposite of what pkg-config-first would give.  That ordering
 * matters: a development machine that also has gowl installed would
 * otherwise compile plugins against the installed headers -- possibly
 * a different wlroots series -- and the mismatch shows up as a
 * segfault at load time rather than a compile error.
 */
static gchar *
registry_include_flags(void)
{
	g_autofree gchar *pkg_out = NULL;
	g_autofree gchar *deps = NULL;
	gint status;

	/* cairo and pango arrive here rather than through crispy's base
	   flags because the plugin headers include them. */
	if (!g_spawn_command_line_sync(
		    "pkg-config --cflags cairo pangocairo", &deps, NULL,
		    &status, NULL) ||
	    !g_spawn_check_wait_status(status, NULL)) {
		g_clear_pointer(&deps, g_free);
		deps = g_strdup("");
	} else {
		g_strstrip(deps);
	}

#ifdef GOWL_DEV_INCLUDE_DIR
	if (g_file_test(GOWL_DEV_INCLUDE_DIR, G_FILE_TEST_IS_DIR)) {
		g_autofree gchar *build_dir = NULL;
		g_autofree gchar *project_dir = NULL;
		g_autofree gchar *crispy_dir = NULL;
		g_autofree gchar *yaml_dir = NULL;

		build_dir   = g_path_get_dirname(GOWL_DEV_INCLUDE_DIR);
		project_dir = g_path_get_dirname(build_dir);
		crispy_dir  = g_build_filename(project_dir, "deps", "crispy",
		                               "src", NULL);
		yaml_dir    = g_build_filename(project_dir, "deps",
		                               "yaml-glib", "src", NULL);

		return g_strdup_printf("-I%s -I%s/gowl -I%s -I%s %s",
		                       GOWL_DEV_INCLUDE_DIR,
		                       GOWL_DEV_INCLUDE_DIR,
		                       crispy_dir, yaml_dir, deps);
	}
#endif

	if (g_spawn_command_line_sync("pkg-config --cflags gowl", &pkg_out,
	                              NULL, &status, NULL) &&
	    g_spawn_check_wait_status(status, NULL) && pkg_out != NULL) {
		g_strstrip(pkg_out);
		if (pkg_out[0] != '\0')
			return g_strdup_printf("%s %s", pkg_out, deps);
	}

	return g_strdup(deps);
}

/* Pull an optional `#define CRISPY_PARAMS "..."' out of a source file,
   the same escape hatch a C configuration file has for extra flags. */
static gchar *
registry_extract_params(const gchar *source)
{
	const gchar *p;
	const gchar *start;
	const gchar *end;

	if (source == NULL)
		return g_strdup("");

	p = strstr(source, "CRISPY_PARAMS");
	if (p == NULL)
		return g_strdup("");

	start = strchr(p, '"');
	if (start == NULL)
		return g_strdup("");
	start++;
	end = strchr(start, '"');
	if (end == NULL)
		return g_strdup("");

	return g_strndup(start, (gsize)(end - start));
}

/* Bring up the crispy toolchain on first use.  A failure is sticky:
   probing gcc once per failed load is noise, and the answer will not
   change within a session. */
static gboolean
registry_ensure_compiler(GowlBarRegistry *self, GError **error)
{
	g_autofree gchar *cache_dir = NULL;

	if (self->compiler != NULL)
		return TRUE;
	if (self->compiler_failed) {
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
		            "no C compiler is available, so .c bar plugins "
		            "cannot be built");
		return FALSE;
	}

	self->compiler = crispy_gcc_compiler_new(error);
	if (self->compiler == NULL) {
		self->compiler_failed = TRUE;
		return FALSE;
	}

	cache_dir = g_build_filename(g_get_user_cache_dir(), "gowl",
	                             "bar-plugins", NULL);
	g_mkdir_with_parents(cache_dir, 0700);
	self->cache = crispy_file_cache_new_with_dir(cache_dir);
	return TRUE;
}

/* Compile a .c plugin to a cached shared object.  Returns the .so
   path. */
static gchar *
registry_compile(GowlBarRegistry *self, const gchar *source_path,
                 gboolean force, GError **error)
{
	g_autofree gchar *source = NULL;
	g_autofree gchar *params = NULL;
	g_autofree gchar *includes = NULL;
	g_autofree gchar *flags = NULL;
	g_autofree gchar *hash = NULL;
	gchar *so_path;

	if (!registry_ensure_compiler(self, error))
		return NULL;

	if (!g_file_get_contents(source_path, &source, NULL, error))
		return NULL;

	params   = registry_extract_params(source);
	includes = registry_include_flags();
	flags    = g_strdup_printf("%s %s", includes, params);

	hash = crispy_cache_provider_compute_hash(
		CRISPY_CACHE_PROVIDER(self->cache), source, -1, flags,
		crispy_compiler_get_version(
			CRISPY_COMPILER(self->compiler)));
	so_path = crispy_cache_provider_get_path(
		CRISPY_CACHE_PROVIDER(self->cache), hash);

	if (!force &&
	    crispy_cache_provider_has_valid(
		CRISPY_CACHE_PROVIDER(self->cache), hash, source_path)) {
		g_debug("gowl-bar: plugin cache hit for %s", source_path);
		return so_path;
	}

	g_message("gowl-bar: compiling plugin %s", source_path);
	if (!crispy_compiler_compile_shared(CRISPY_COMPILER(self->compiler),
	                                    source_path, so_path, flags,
	                                    error)) {
		g_free(so_path);
		return NULL;
	}
	return so_path;
}

/* Register everything one opened module publishes. */
static gint
registry_take_descs(GowlBarRegistry *self, GModule *module,
                    const gchar *source_path, const gchar *so_path,
                    GError **error)
{
	gpointer symbol;
	GowlBarPluginQueryFunc query;
	const GowlBarPluginDesc *descs;
	guint n_descs, i;
	gint registered;

	if (!g_module_symbol(module, GOWL_BAR_PLUGIN_QUERY_SYMBOL, &symbol) ||
	    symbol == NULL) {
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
		            "'%s' exports no %s(); it is not a bar plugin",
		            so_path, GOWL_BAR_PLUGIN_QUERY_SYMBOL);
		return 0;
	}

	query = (GowlBarPluginQueryFunc)symbol;
	n_descs = 0;
	descs = query(&n_descs);
	if (descs == NULL || n_descs == 0) {
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
		            "'%s' published no plugins", so_path);
		return 0;
	}

	registered = 0;
	for (i = 0; i < n_descs; i++) {
		const GowlBarPluginDesc *desc = &descs[i];
		RegistryEntry *entry;

		if (desc->abi != GOWL_BAR_PLUGIN_ABI) {
			g_warning("gowl-bar: '%s' entry %u was built for "
			          "plugin ABI %u, this gowl speaks %d -- "
			          "skipping it",
			          so_path, i, desc->abi,
			          GOWL_BAR_PLUGIN_ABI);
			continue;
		}
		if (desc->name == NULL || desc->name[0] == '\0') {
			g_warning("gowl-bar: '%s' entry %u has no name",
			          so_path, i);
			continue;
		}
		if (desc->vtable == NULL && desc->factory == NULL) {
			g_warning("gowl-bar: plugin '%s' from '%s' has "
			          "neither a vtable nor a factory",
			          desc->name, so_path);
			continue;
		}

		/* Replacing an existing name is how a reload works, and is
		   also how a user's plugin directory deliberately shadows a
		   built-in.  Say so once rather than refusing. */
		if (g_hash_table_contains(self->entries, desc->name)) {
			g_message("gowl-bar: '%s' replaces the previously "
			          "registered plugin of that name",
			          desc->name);
			g_signal_emit(self,
				registry_signals[SIGNAL_PLUGIN_UNLOADED], 0,
				desc->name);
		}

		entry = registry_entry_new(desc->name, desc->title,
		                           desc->description);
		entry->version     = g_strdup(desc->version);
		entry->vtable      = desc->vtable;
		entry->factory     = desc->factory;
		entry->module      = module;
		entry->source_path = g_strdup(source_path);
		entry->builtin     = FALSE;
		registry_insert(self, entry);
		registered++;
	}

	if (registered == 0) {
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
		            "'%s' published nothing this gowl can use",
		            so_path);
	}
	return registered;
}

/* The load body, run under the fault guard. */
typedef struct {
	GowlBarRegistry *registry;
	const gchar     *source_path;
	gchar           *so_path;
	gint             registered;
	GError          *error;
} LoadCtx;

static void
registry_load_body(gpointer data)
{
	LoadCtx *ctx = data;
	GModule *module;

	module = g_module_open(ctx->so_path, G_MODULE_BIND_LAZY);
	if (module == NULL) {
		g_set_error(&ctx->error, G_IO_ERROR, G_IO_ERROR_FAILED,
		            "could not open '%s': %s", ctx->so_path,
		            g_module_error());
		return;
	}

	/* Resident on purpose.  See gowl_bar_registry_unload(). */
	g_module_make_resident(module);

	ctx->registered = registry_take_descs(ctx->registry, module,
	                                      ctx->source_path,
	                                      ctx->so_path, &ctx->error);
}

/**
 * gowl_bar_registry_load_file:
 * @self: a registry
 * @path: a `.so' or `.c' plugin
 * @error: (nullable): return location for a #GError
 *
 * Returns: %TRUE when at least one plugin was registered
 */
gboolean
gowl_bar_registry_load_file(GowlBarRegistry *self, const gchar *path,
                            GError **error)
{
	g_autofree gchar *basename = NULL;
	g_autofree gchar *stem = NULL;
	g_autofree gchar *so_path = NULL;
	LoadCtx ctx;
	gint signo;
	gboolean survived;

	g_return_val_if_fail(GOWL_IS_BAR_REGISTRY(self), FALSE);
	g_return_val_if_fail(path != NULL, FALSE);

	basename = g_path_get_basename(path);
	if (g_str_has_suffix(basename, ".so"))
		stem = g_strndup(basename, strlen(basename) - 3);
	else if (g_str_has_suffix(basename, ".c"))
		stem = g_strndup(basename, strlen(basename) - 2);
	else
		stem = g_strdup(basename);

	if (gowl_bar_registry_is_quarantined(self, stem)) {
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
		            "'%s' is quarantined: %s", stem,
		            gowl_bar_registry_quarantine_reason(self, stem));
		return FALSE;
	}

	if (g_str_has_suffix(path, ".c")) {
		so_path = registry_compile(self, path, FALSE, error);
		if (so_path == NULL)
			return FALSE;
	} else {
		so_path = g_strdup(path);
	}

	/* From here to journal_end, a crash is attributable to this
	   plugin.  That window is the whole reason the journal exists. */
	journal_begin(self, stem, path);

	memset(&ctx, 0, sizeof(ctx));
	ctx.registry    = self;
	ctx.source_path = path;
	ctx.so_path     = so_path;

	survived = gowl_bar_guard_call(registry_load_body, &ctx, &signo,
	                               "a bar plugin load");
	if (!survived) {
		g_autofree gchar *reason = NULL;

		reason = g_strdup_printf("it raised %s while loading",
		                         gowl_bar_guard_signal_name(signo));
		gowl_bar_registry_quarantine(self, stem, reason);
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
		            "plugin '%s' faulted while loading (%s)", stem,
		            gowl_bar_guard_signal_name(signo));
		g_clear_error(&ctx.error);
		return FALSE;
	}

	journal_end(self, stem);

	if (ctx.registered == 0) {
		if (ctx.error != NULL)
			g_propagate_error(error, ctx.error);
		else
			g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
			            "'%s' registered nothing", path);
		g_signal_emit(self, registry_signals[SIGNAL_PLUGIN_FAILED], 0,
		              stem,
		              (error != NULL && *error != NULL)
		              	? (*error)->message : "load failed");
		return FALSE;
	}

	g_clear_error(&ctx.error);
	return TRUE;
}

/**
 * gowl_bar_registry_load_directory:
 * @self: a registry
 * @dir_path: a directory to scan
 *
 * Returns: how many files loaded successfully
 */
gint
gowl_bar_registry_load_directory(GowlBarRegistry *self, const gchar *dir_path)
{
	g_autoptr(GDir) dir = NULL;
	g_autoptr(GPtrArray) files = NULL;
	const gchar *name;
	guint i;
	gint loaded;

	g_return_val_if_fail(GOWL_IS_BAR_REGISTRY(self), 0);

	if (dir_path == NULL)
		return 0;

	dir = g_dir_open(dir_path, 0, NULL);
	if (dir == NULL)
		return 0;

	files = g_ptr_array_new_with_free_func(g_free);
	while ((name = g_dir_read_name(dir)) != NULL) {
		if (!g_str_has_suffix(name, ".so") &&
		    !g_str_has_suffix(name, ".c"))
			continue;
		g_ptr_array_add(files, g_build_filename(dir_path, name, NULL));
	}

	/* Sorted so a directory of plugins loads in a predictable order;
	   two plugins claiming one name must resolve the same way every
	   start, not according to readdir. */
	g_ptr_array_sort(files, registry_cmp_names);

	loaded = 0;
	for (i = 0; i < files->len; i++) {
		g_autoptr(GError) error = NULL;
		const gchar *file = g_ptr_array_index(files, i);

		if (gowl_bar_registry_load_file(self, file, &error)) {
			loaded++;
		} else {
			g_warning("gowl-bar: could not load '%s': %s", file,
			          error->message);
		}
	}
	return loaded;
}

/**
 * gowl_bar_registry_unload:
 * @self: a registry
 * @name: a registered name
 * @error: (nullable): return location for a #GError
 *
 * Returns: %TRUE when @name was removed
 */
gboolean
gowl_bar_registry_unload(GowlBarRegistry *self, const gchar *name,
                         GError **error)
{
	const gchar *resolved;

	g_return_val_if_fail(GOWL_IS_BAR_REGISTRY(self), FALSE);
	g_return_val_if_fail(name != NULL, FALSE);

	resolved = registry_resolve(self, name);
	if (!g_hash_table_contains(self->entries, resolved)) {
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
		            "no bar plugin named '%s'", name);
		return FALSE;
	}

	/* Emitted before the entry dies: the host's handler drops live
	   instances, which must happen while their vtable is still a
	   valid pointer. */
	g_signal_emit(self, registry_signals[SIGNAL_PLUGIN_UNLOADED], 0,
	              resolved);
	g_hash_table_remove(self->entries, resolved);
	return TRUE;
}

/**
 * gowl_bar_registry_reload:
 * @self: a registry
 * @name: a registered name
 * @error: (nullable): return location for a #GError
 *
 * Returns: %TRUE on success
 */
gboolean
gowl_bar_registry_reload(GowlBarRegistry *self, const gchar *name,
                         GError **error)
{
	RegistryEntry *entry;
	g_autofree gchar *source_path = NULL;
	const gchar *resolved;

	g_return_val_if_fail(GOWL_IS_BAR_REGISTRY(self), FALSE);
	g_return_val_if_fail(name != NULL, FALSE);

	resolved = registry_resolve(self, name);
	entry = g_hash_table_lookup(self->entries, resolved);
	if (entry == NULL) {
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
		            "no bar plugin named '%s'", name);
		return FALSE;
	}
	if (entry->builtin || entry->source_path == NULL) {
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
		            "'%s' is built in and has no file to reload",
		            resolved);
		return FALSE;
	}

	source_path = g_strdup(entry->source_path);

	/* Drop the entry first so live instances are released before the
	   replacement registers; the reload then looks exactly like a
	   first load to everything downstream. */
	g_signal_emit(self, registry_signals[SIGNAL_PLUGIN_UNLOADED], 0,
	              resolved);
	g_hash_table_remove(self->entries, resolved);

	return gowl_bar_registry_load_file(self, source_path, error);
}
