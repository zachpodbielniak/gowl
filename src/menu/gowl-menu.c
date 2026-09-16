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
 * The menu model.  See gowl-menu.h for what this is and is not.
 *
 * Three things live here and nothing else does:
 *
 *   THE TREE, read from YAML.  Nesting is real nesting rather than
 *   omarchy's dotted keys, because YAML already has a way to say "these
 *   belong to that" and a second one invented on top of it is a second
 *   one to keep in sync.  Routes are still dotted, because a route is a
 *   thing people type.
 *
 *   THE GUARDS, answered in this process.  A guard is a question about
 *   the machine (is nmcli installed) or about the session (what is the
 *   layout), and the session half is answered by asking the compositor
 *   the same question a script would.  That is the whole trick: the IPC
 *   surface is already a complete vocabulary for the session's state, so
 *   the menu needs no vocabulary of its own and gains every command any
 *   module adds for free.
 *
 *   THE ACTIONS, run through the keybind dispatcher.  A row IS a keybind
 *   without a key -- same action enum, same argument, same code path --
 *   so anything bindable is menu-able and the two can never drift.
 */

#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "gowl-menu"

#include "menu/gowl-menu.h"

#include "core/gowl-core-private.h"
#include "core/gowl-compositor.h"
#include "core/gowl-monitor.h"
#include "core/gowl-client.h"
#include "core/gowl-layout-registry.h"
#include "config/gowl-config.h"
#include "config/gowl-keybind.h"
#include "module/gowl-module-manager.h"
#include "tray/gowl-tray.h"
#include "gowl-enums.h"

#include <gio/gio.h>
#include <string.h>

/* yaml-glib headers -- available via -Ideps/yaml-glib/src */
#include "yaml-glib.h"

/*
 * How deep a tree may nest.
 *
 * Not an arithmetic limit: it is a guard against a `target:' that points
 * at an ancestor, which is a cycle, and against a hand-written file
 * nested past the point where the breadcrumb stops fitting.  Six levels
 * is two more than anything shipped uses.
 */
#define GOWL_MENU_MAX_DEPTH (6)

/* ────────────────────────────────────────────────────────────────────
 * Guards
 * ──────────────────────────────────────────────────────────────────── */

typedef struct {
	GowlMenuGuardKind kind;
	gchar            *key;    /* the program, path, module or query */
	gchar            *is;     /* what the answer must be, or NULL */
	gboolean          truth;  /* for GOWL_MENU_GUARD_CONST */
	gboolean          negate;
} GowlMenuGuard;

static void
guard_clear(GowlMenuGuard *g)
{
	g_clear_pointer(&g->key, g_free);
	g_clear_pointer(&g->is, g_free);
	g->kind = GOWL_MENU_GUARD_NONE;
	g->truth = FALSE;
	g->negate = FALSE;
}

/* ────────────────────────────────────────────────────────────────────
 * Entries
 * ──────────────────────────────────────────────────────────────────── */

typedef struct _GowlMenuEntry GowlMenuEntry;

struct _GowlMenuEntry {
	gchar      *id;        /* the last route segment */
	gchar      *route;     /* the whole dotted route */
	gchar      *icon;
	gchar      *label;
	gchar      *title;     /* header when open; NULL means use the label */
	gchar      *detail;    /* description: a second line, and search text */
	GStrv       aliases;

	gboolean    has_action;
	GowlAction  action;
	gchar      *arg;

	gchar      *target;    /* a link to another route */
	gchar      *provider;  /* rows produced at open time */
	gboolean    keep_open;

	GowlMenuGuard when;
	GowlMenuGuard checked;
	GowlMenuGuard disabled;

	GPtrArray  *children;  /* GowlMenuEntry *, owned */
};

static void
entry_free(gpointer data)
{
	GowlMenuEntry *e = data;

	if (e == NULL)
		return;
	g_free(e->id);
	g_free(e->route);
	g_free(e->icon);
	g_free(e->label);
	g_free(e->title);
	g_free(e->detail);
	g_strfreev(e->aliases);
	g_free(e->arg);
	g_free(e->target);
	g_free(e->provider);
	guard_clear(&e->when);
	guard_clear(&e->checked);
	guard_clear(&e->disabled);
	g_clear_pointer(&e->children, g_ptr_array_unref);
	g_free(e);
}

static GowlMenuEntry *
entry_new(const gchar *id, const gchar *route)
{
	GowlMenuEntry *e = g_new0(GowlMenuEntry, 1);

	e->id       = g_strdup(id);
	e->route    = g_strdup(route);
	e->children = g_ptr_array_new_with_free_func(entry_free);
	return e;
}

/* ────────────────────────────────────────────────────────────────────
 * Rows
 * ──────────────────────────────────────────────────────────────────── */

GowlMenuRow *
gowl_menu_row_copy(const GowlMenuRow *self)
{
	GowlMenuRow *copy;

	if (self == NULL)
		return NULL;
	copy = g_new0(GowlMenuRow, 1);
	copy->route     = g_strdup(self->route);
	copy->icon      = g_strdup(self->icon);
	copy->icon_name = g_strdup(self->icon_name);
	copy->label    = g_strdup(self->label);
	copy->detail   = g_strdup(self->detail);
	copy->value    = g_strdup(self->value);
	copy->submenu  = self->submenu;
	copy->runnable = self->runnable;
	copy->checked  = self->checked;
	copy->disabled = self->disabled;
	copy->children = self->children;
	return copy;
}

void
gowl_menu_row_free(GowlMenuRow *self)
{
	if (self == NULL)
		return;
	g_free(self->route);
	g_free(self->icon);
	g_free(self->icon_name);
	g_free(self->label);
	g_free(self->detail);
	g_free(self->value);
	g_free(self);
}

G_DEFINE_BOXED_TYPE(GowlMenuRow, gowl_menu_row,
                    gowl_menu_row_copy, gowl_menu_row_free)

/*
 * A row a provider made up, before it becomes a #GowlMenuRow.
 *
 * Providers carry their own action rather than deriving one from the
 * submenu's, because the interesting providers do not agree about what
 * activating a row means: an app spawns, a window is focused, a layout
 * is set.  One field costs less than three special cases.
 */
typedef struct {
	gchar      *id;
	gchar      *icon;
	gchar      *icon_name;
	gchar      *label;
	gchar      *detail;
	gchar      *value;
	gboolean    checked;
	GowlAction  action;
	gchar      *arg;
} GowlMenuProviderRow;

static void
provider_row_free(gpointer data)
{
	GowlMenuProviderRow *r = data;

	if (r == NULL)
		return;
	g_free(r->id);
	g_free(r->icon);
	g_free(r->icon_name);
	g_free(r->label);
	g_free(r->detail);
	g_free(r->value);
	g_free(r->arg);
	g_free(r);
}

/* ────────────────────────────────────────────────────────────────────
 * The object
 * ──────────────────────────────────────────────────────────────────── */

struct _GowlMenu {
	GObject        parent_instance;

	GowlMenuEntry *root;
	GHashTable    *by_route;   /* route -> GowlMenuEntry *, unowned */
	GHashTable    *by_alias;   /* alias -> route, both owned */
	gchar         *source;
	guint          serial;

	/*
	 * Provider results worth keeping.  Only the expensive one is:
	 * enumerating desktop entries costs about 18ms every time -- GIO
	 * does not memoise it -- and search runs providers on every
	 * keystroke, on the compositor thread.  18ms a keystroke is a
	 * desktop that stutters while you type.
	 *
	 * Everything else is cheap and must be FRESH: a window list five
	 * seconds stale is a list of windows that may not be there.
	 */
	GHashTable    *provider_cache;   /* name -> CachedRows *, owned */
};

typedef struct {
	GPtrArray *rows;
	gint64     when;   /* g_get_monotonic_time() */
} CachedRows;

static void
cached_rows_free(gpointer data)
{
	CachedRows *c = data;

	if (c == NULL)
		return;
	g_clear_pointer(&c->rows, g_ptr_array_unref);
	g_free(c);
}

/*
 * How long a provider's rows may be reused, in microseconds.
 *
 * Zero for everything that describes the session as it is right now.
 * The application list is the exception: it changes when something is
 * installed, which is not something that happens between two
 * keystrokes, and re-reading every desktop entry on the machine to
 * answer one is the only part of this that is slow enough to see.
 */
static gint64
provider_ttl(const gchar *name)
{
	if (g_strcmp0(name, "apps") == 0)
		return 5 * G_USEC_PER_SEC;
	return 0;
}

G_DEFINE_TYPE(GowlMenu, gowl_menu, G_TYPE_OBJECT)

static void
gowl_menu_finalize(GObject *object)
{
	GowlMenu *self = GOWL_MENU(object);

	g_clear_pointer(&self->by_route, g_hash_table_unref);
	g_clear_pointer(&self->by_alias, g_hash_table_unref);
	g_clear_pointer(&self->provider_cache, g_hash_table_unref);
	g_clear_pointer(&self->root, entry_free);
	g_clear_pointer(&self->source, g_free);
	G_OBJECT_CLASS(gowl_menu_parent_class)->finalize(object);
}

static void
gowl_menu_class_init(GowlMenuClass *klass)
{
	G_OBJECT_CLASS(klass)->finalize = gowl_menu_finalize;
}

static void
gowl_menu_init(GowlMenu *self)
{
	self->root     = entry_new(NULL, NULL);
	self->by_route = g_hash_table_new(g_str_hash, g_str_equal);
	self->by_alias = g_hash_table_new_full(g_str_hash, g_str_equal,
	                                       g_free, g_free);
	self->provider_cache = g_hash_table_new_full(g_str_hash, g_str_equal,
	                                             g_free, cached_rows_free);
}

GowlMenu *
gowl_menu_new(void)
{
	return g_object_new(GOWL_TYPE_MENU, NULL);
}

GowlMenu *
gowl_menu_get_default(void)
{
	static GowlMenu *singleton;

	if (singleton == NULL) {
		g_autoptr(GError) error = NULL;

		singleton = gowl_menu_new();
		if (!gowl_menu_load(singleton, &error)) {
			g_message("no menu definition found (%s); the menu is empty",
			          error != NULL ? error->message : "no reason given");
		}
	}
	return singleton;
}

guint
gowl_menu_get_serial(GowlMenu *self)
{
	g_return_val_if_fail(GOWL_IS_MENU(self), 0);
	return self->serial;
}

const gchar *
gowl_menu_get_source(GowlMenu *self)
{
	g_return_val_if_fail(GOWL_IS_MENU(self), NULL);
	return self->source;
}

/* ────────────────────────────────────────────────────────────────────
 * Parsing
 * ──────────────────────────────────────────────────────────────────── */

/*
 * A label turned into a route segment.
 *
 * Only used when an entry declares no `id:'.  Lower case, spaces and
 * punctuation collapsed to single dashes, because the result is
 * something people type at `gowl menu summon'.
 */
static gchar *
slugify(const gchar *text)
{
	GString *out;
	const gchar *p;
	gboolean dash = FALSE;

	if (text == NULL || *text == '\0')
		return g_strdup("item");

	out = g_string_new(NULL);
	for (p = text; *p != '\0'; p++) {
		if (g_ascii_isalnum(*p)) {
			g_string_append_c(out, g_ascii_tolower(*p));
			dash = FALSE;
		} else if (!dash && out->len > 0) {
			g_string_append_c(out, '-');
			dash = TRUE;
		}
	}
	/* A trailing dash comes from trailing punctuation and is never
	 * what anybody means by the name. */
	while (out->len > 0 && out->str[out->len - 1] == '-')
		g_string_truncate(out, out->len - 1);
	if (out->len == 0)
		g_string_assign(out, "item");
	return g_string_free(out, FALSE);
}

/* Route comparison: case-insensitive, and `_' reads as `-' so a route
 * typed either way finds the same entry. */
static gchar *
route_normalise(const gchar *route)
{
	gchar *norm;

	if (route == NULL)
		return NULL;
	norm = g_ascii_strdown(route, -1);
	g_strdelimit(norm, "_", '-');
	return norm;
}

static gboolean
scalar_is_true(const gchar *text)
{
	return text != NULL
	       && (g_ascii_strcasecmp(text, "true") == 0
	           || g_ascii_strcasecmp(text, "yes") == 0
	           || g_ascii_strcasecmp(text, "on") == 0
	           || g_strcmp0(text, "1") == 0);
}

/*
 * One guard out of one key.
 *
 * Written either as a mapping -- `when: {exists: nmcli}' -- or as a bare
 * boolean, which is how a row is switched off without deleting it.
 */
static void
parse_guard(YamlMapping *map, const gchar *key, GowlMenuGuard *out)
{
	YamlNode    *node;
	YamlMapping *guard;
	const gchar *scalar;

	if (map == NULL || !yaml_mapping_has_member(map, key))
		return;

	node = yaml_mapping_get_member(map, key);
	if (node == NULL)
		return;

	if (yaml_node_get_node_type(node) == YAML_NODE_SCALAR) {
		scalar = yaml_node_get_scalar(node);
		guard_clear(out);
		out->kind  = GOWL_MENU_GUARD_CONST;
		out->truth = scalar_is_true(scalar);
		return;
	}

	guard = yaml_node_get_mapping(node);
	if (guard == NULL)
		return;

	guard_clear(out);
	if (yaml_mapping_has_member(guard, "exists")) {
		out->kind = GOWL_MENU_GUARD_EXISTS;
		out->key  = g_strdup(yaml_mapping_get_string_member(guard, "exists"));
	} else if (yaml_mapping_has_member(guard, "file")) {
		out->kind = GOWL_MENU_GUARD_FILE;
		out->key  = g_strdup(yaml_mapping_get_string_member(guard, "file"));
	} else if (yaml_mapping_has_member(guard, "module")) {
		out->kind = GOWL_MENU_GUARD_MODULE;
		out->key  = g_strdup(yaml_mapping_get_string_member(guard, "module"));
	} else if (yaml_mapping_has_member(guard, "embedder")) {
		out->kind  = GOWL_MENU_GUARD_EMBEDDER;
		out->truth = yaml_mapping_get_boolean_member(guard, "embedder");
		/* `embedder: false' is the same question asked the other way
		 * round, which is how a row is shown ONLY in standalone gowl. */
		if (!out->truth)
			out->negate = !out->negate;
	} else if (yaml_mapping_has_member(guard, "ipc")) {
		out->kind = GOWL_MENU_GUARD_IPC;
		out->key  = g_strdup(yaml_mapping_get_string_member(guard, "ipc"));
	} else {
		g_warning("menu: guard `%s' names none of exists, file, module, "
		          "embedder or ipc", key);
		return;
	}

	if (yaml_mapping_has_member(guard, "is"))
		out->is = g_strdup(yaml_mapping_get_string_member(guard, "is"));
	if (yaml_mapping_has_member(guard, "not")
	    && yaml_mapping_get_boolean_member(guard, "not"))
		out->negate = !out->negate;
}

static gboolean
parse_action(YamlMapping *map, GowlAction *out_action, gchar **out_arg)
{
	GEnumClass  *klass;
	GEnumValue  *val;
	const gchar *name;
	g_autofree gchar *norm = NULL;

	/*
	 * The three shorthands first.  They exist because they are what
	 * the shipped tree is mostly made of, and `spawn: firefox' reads
	 * as what it does where `action: spawn' plus `arg: firefox' reads
	 * as a form to be filled in.
	 */
	if (yaml_mapping_has_member(map, "spawn")) {
		*out_action = GOWL_ACTION_SPAWN;
		*out_arg    = g_strdup(yaml_mapping_get_string_member(map, "spawn"));
		return TRUE;
	}
	if (yaml_mapping_has_member(map, "command")) {
		*out_action = GOWL_ACTION_IPC_COMMAND;
		*out_arg    = g_strdup(yaml_mapping_get_string_member(map, "command"));
		return TRUE;
	}
	if (yaml_mapping_has_member(map, "elisp")) {
		*out_action = GOWL_ACTION_CUSTOM;
		*out_arg    = g_strdup(yaml_mapping_get_string_member(map, "elisp"));
		return TRUE;
	}
	if (!yaml_mapping_has_member(map, "action"))
		return FALSE;

	name = yaml_mapping_get_string_member(map, "action");
	if (name == NULL)
		return FALSE;
	norm = g_strdup(name);
	g_strdelimit(norm, "_", '-');

	klass = (GEnumClass *)g_type_class_ref(GOWL_TYPE_ACTION);
	val   = g_enum_get_value_by_nick(klass, norm);
	g_type_class_unref(klass);
	if (val == NULL) {
		g_warning("menu: unknown action `%s'", name);
		return FALSE;
	}

	*out_action = val->value;
	*out_arg    = yaml_mapping_has_member(map, "arg")
		? g_strdup(yaml_mapping_get_string_member(map, "arg"))
		: NULL;
	return TRUE;
}

static void parse_items(GowlMenu *self, YamlSequence *seq,
                        GowlMenuEntry *parent, gint depth);

/*
 * Apply one mapping onto @e.
 *
 * Every field is optional, and a field that is absent is LEFT ALONE
 * rather than reset.  That is what makes the user's file an overlay: a
 * three-line entry that renames a shipped row keeps the row's icon, its
 * guards and its action, and keeps its place in the list.
 */
static void
apply_entry(GowlMenu *self, YamlMapping *map, GowlMenuEntry *e, gint depth)
{
	if (yaml_mapping_has_member(map, "icon")) {
		g_free(e->icon);
		e->icon = g_strdup(yaml_mapping_get_string_member(map, "icon"));
	}
	if (yaml_mapping_has_member(map, "label")) {
		g_free(e->label);
		e->label = g_strdup(yaml_mapping_get_string_member(map, "label"));
	}
	if (yaml_mapping_has_member(map, "title")) {
		g_free(e->title);
		e->title = g_strdup(yaml_mapping_get_string_member(map, "title"));
	}
	if (yaml_mapping_has_member(map, "desc")) {
		g_free(e->detail);
		e->detail = g_strdup(yaml_mapping_get_string_member(map, "desc"));
	}
	if (yaml_mapping_has_member(map, "target")) {
		g_free(e->target);
		e->target = g_strdup(yaml_mapping_get_string_member(map, "target"));
	}
	if (yaml_mapping_has_member(map, "provider")) {
		g_free(e->provider);
		e->provider = g_strdup(yaml_mapping_get_string_member(map, "provider"));
	}
	if (yaml_mapping_has_member(map, "keep-open"))
		e->keep_open = yaml_mapping_get_boolean_member(map, "keep-open");

	if (yaml_mapping_has_member(map, "aliases")) {
		YamlSequence *seq = yaml_mapping_get_sequence_member(map, "aliases");

		if (seq != NULL) {
			guint len = yaml_sequence_get_length(seq);
			guint i;
			GPtrArray *out = g_ptr_array_new();

			for (i = 0; i < len; i++) {
				const gchar *s = yaml_sequence_get_string_element(seq, i);

				if (s != NULL && *s != '\0')
					g_ptr_array_add(out, g_strdup(s));
			}
			g_ptr_array_add(out, NULL);
			g_strfreev(e->aliases);
			e->aliases = (GStrv)g_ptr_array_free(out, FALSE);
		}
	}

	{
		GowlAction action = GOWL_ACTION_NONE;
		g_autofree gchar *arg = NULL;

		if (parse_action(map, &action, &arg)) {
			e->has_action = TRUE;
			e->action     = action;
			g_free(e->arg);
			e->arg        = g_steal_pointer(&arg);
		}
	}

	parse_guard(map, "when", &e->when);
	parse_guard(map, "checked", &e->checked);
	parse_guard(map, "disabled", &e->disabled);

	if (yaml_mapping_has_member(map, "items")) {
		YamlSequence *seq = yaml_mapping_get_sequence_member(map, "items");

		if (seq != NULL)
			parse_items(self, seq, e, depth + 1);
	}
}

static GowlMenuEntry *
child_by_id(GowlMenuEntry *parent, const gchar *id)
{
	guint i;

	for (i = 0; parent != NULL && i < parent->children->len; i++) {
		GowlMenuEntry *c = g_ptr_array_index(parent->children, i);

		if (g_strcmp0(c->id, id) == 0)
			return c;
	}
	return NULL;
}

static void
parse_items(GowlMenu *self, YamlSequence *seq, GowlMenuEntry *parent,
            gint depth)
{
	guint len;
	guint i;

	if (depth > GOWL_MENU_MAX_DEPTH) {
		g_warning("menu: tree deeper than %d levels at `%s'; stopping",
		          GOWL_MENU_MAX_DEPTH,
		          parent->route != NULL ? parent->route : "root");
		return;
	}

	len = yaml_sequence_get_length(seq);
	for (i = 0; i < len; i++) {
		YamlMapping *map = yaml_sequence_get_mapping_element(seq, i);
		g_autofree gchar *id = NULL;
		GowlMenuEntry *e;

		if (map == NULL)
			continue;

		if (yaml_mapping_has_member(map, "id"))
			id = g_strdup(yaml_mapping_get_string_member(map, "id"));
		if (id == NULL || *id == '\0') {
			g_free(id);
			id = slugify(yaml_mapping_has_member(map, "label")
			             ? yaml_mapping_get_string_member(map, "label")
			             : NULL);
		}

		e = child_by_id(parent, id);
		if (e == NULL) {
			g_autofree gchar *route = parent->route != NULL
				? g_strdup_printf("%s.%s", parent->route, id)
				: g_strdup(id);

			e = entry_new(id, route);
			if (e->label == NULL)
				e->label = g_strdup(id);
			g_ptr_array_add(parent->children, e);
		}
		apply_entry(self, map, e, depth);
	}
}

/* Rebuild the route and alias indexes after a load. */
static void
reindex_entry(GowlMenu *self, GowlMenuEntry *e)
{
	guint i;

	if (e->route != NULL) {
		g_autofree gchar *norm = route_normalise(e->route);

		g_hash_table_replace(self->by_route, e->route, e);
		/* The normalised spelling too, so `Style.Backdrop' and
		 * `style_backdrop' both land without a second lookup path. */
		if (g_strcmp0(norm, e->route) != 0)
			g_hash_table_replace(self->by_alias,
			                     g_steal_pointer(&norm),
			                     g_strdup(e->route));
		if (e->aliases != NULL) {
			gint a;

			for (a = 0; e->aliases[a] != NULL; a++) {
				gchar *alias = route_normalise(e->aliases[a]);

				g_hash_table_replace(self->by_alias, alias,
				                     g_strdup(e->route));
			}
		}
		/* The leaf segment is an alias too: `backdrop' reaches
		 * `style.backdrop' when nothing else claims the word.  Declared
		 * routes and declared aliases both win over this, which is why
		 * it is inserted only when the key is free. */
		if (!g_hash_table_contains(self->by_alias, e->id)
		    && !g_hash_table_contains(self->by_route, e->id)) {
			g_hash_table_replace(self->by_alias,
			                     route_normalise(e->id),
			                     g_strdup(e->route));
		}
	}

	for (i = 0; i < e->children->len; i++)
		reindex_entry(self, g_ptr_array_index(e->children, i));
}

static void
reindex(GowlMenu *self)
{
	g_hash_table_remove_all(self->by_route);
	g_hash_table_remove_all(self->by_alias);
	reindex_entry(self, self->root);
	g_hash_table_remove_all(self->provider_cache);
	self->serial++;
}

static gboolean
load_root_mapping(GowlMenu *self, YamlMapping *mapping, gboolean merge,
                  const gchar *what, GError **error)
{
	YamlSequence *seq;

	if (mapping == NULL || !yaml_mapping_has_member(mapping, "menu")) {
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
		            "%s has no top-level `menu:' sequence", what);
		return FALSE;
	}
	seq = yaml_mapping_get_sequence_member(mapping, "menu");
	if (seq == NULL) {
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
		            "%s: `menu:' is not a sequence", what);
		return FALSE;
	}

	if (!merge) {
		/* The indexes point INTO the tree, so they go first: freeing
		 * the root while they still hold its routes leaves every key
		 * in both tables dangling for as long as the parse runs. */
		g_hash_table_remove_all(self->by_route);
		g_hash_table_remove_all(self->by_alias);
		g_clear_pointer(&self->root, entry_free);
		self->root = entry_new(NULL, NULL);
	}
	parse_items(self, seq, self->root, 1);
	reindex(self);
	return TRUE;
}

gboolean
gowl_menu_load_data(GowlMenu *self, const gchar *yaml, gboolean merge,
                    GError **error)
{
	g_autoptr(YamlParser) parser = NULL;
	YamlNode *root;

	g_return_val_if_fail(GOWL_IS_MENU(self), FALSE);
	g_return_val_if_fail(yaml != NULL, FALSE);

	parser = yaml_parser_new();
	if (!yaml_parser_load_from_data(parser, yaml, -1, error))
		return FALSE;
	root = yaml_parser_get_root(parser);
	if (root == NULL || yaml_node_get_node_type(root) != YAML_NODE_MAPPING) {
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
		            "menu document is not a mapping");
		return FALSE;
	}
	return load_root_mapping(self, yaml_node_get_mapping(root), merge,
	                         "menu document", error);
}

gboolean
gowl_menu_load_file(GowlMenu *self, const gchar *path, gboolean merge,
                    GError **error)
{
	g_autoptr(YamlParser) parser = NULL;
	YamlNode *root;

	g_return_val_if_fail(GOWL_IS_MENU(self), FALSE);
	g_return_val_if_fail(path != NULL, FALSE);

	parser = yaml_parser_new();
	if (!yaml_parser_load_from_file(parser, path, error))
		return FALSE;
	root = yaml_parser_get_root(parser);
	if (root == NULL || yaml_node_get_node_type(root) != YAML_NODE_MAPPING) {
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
		            "%s: root is not a mapping", path);
		return FALSE;
	}
	if (!load_root_mapping(self, yaml_node_get_mapping(root), merge,
	                       path, error))
		return FALSE;

	if (self->source == NULL) {
		self->source = g_strdup(path);
	} else {
		gchar *joined = g_strdup_printf("%s:%s", self->source, path);

		g_free(self->source);
		self->source = joined;
	}
	return TRUE;
}

/*
 * Where the shipped tree is looked for.
 *
 * Spelled out rather than taken from GOWL_DATADIR because this file is
 * also compiled into a tree that has not been installed, and because a
 * distribution that moves the prefix still wants the in-tree copy to
 * work when somebody runs the binary out of build/.
 */
static gchar *
find_system_menu(void)
{
	const gchar * const dirs[] = {
		GOWL_DATADIR "/gowl",
		"/usr/local/share/gowl",
		"/usr/share/gowl",
		NULL
	};
	const gchar *env;
	gint i;

	env = g_getenv("GOWL_MENU_FILE");
	if (env != NULL && *env != '\0' && g_file_test(env, G_FILE_TEST_EXISTS))
		return g_strdup(env);

	for (i = 0; dirs[i] != NULL; i++) {
		g_autofree gchar *path = g_build_filename(dirs[i], "menu.yaml", NULL);

		if (g_file_test(path, G_FILE_TEST_EXISTS))
			return g_steal_pointer(&path);
	}

	/*
	 * The in-tree copy, for a development build that has not been
	 * installed.
	 *
	 * An ABSOLUTE path baked in at compile time, not `data/menu.yaml'
	 * relative to the working directory: the process doing the looking
	 * is not always gowl.  Under `cmacs --gowl' the compositor is the
	 * editor, started from the editor's tree, and a relative path then
	 * finds nothing -- which does not fail, it just leaves the menu
	 * empty, and an empty menu is a key that does nothing.
	 */
#ifdef GOWL_DEV_DATADIR
	{
		g_autofree gchar *path = g_build_filename(GOWL_DEV_DATADIR,
		                                          "menu.yaml", NULL);

		if (g_file_test(path, G_FILE_TEST_EXISTS))
			return g_steal_pointer(&path);
	}
#endif
	{
		g_autofree gchar *path = g_build_filename("data", "menu.yaml", NULL);

		if (g_file_test(path, G_FILE_TEST_EXISTS))
			return g_steal_pointer(&path);
	}
	return NULL;
}

static gchar *
user_menu_path(void)
{
	return g_build_filename(g_get_user_config_dir(), "gowl", "menu.yaml",
	                        NULL);
}

gboolean
gowl_menu_load(GowlMenu *self, GError **error)
{
	g_autofree gchar *system_path = NULL;
	g_autofree gchar *user_path = NULL;
	gboolean loaded = FALSE;

	g_return_val_if_fail(GOWL_IS_MENU(self), FALSE);

	g_clear_pointer(&self->source, g_free);
	g_hash_table_remove_all(self->by_route);
	g_hash_table_remove_all(self->by_alias);
	g_clear_pointer(&self->root, entry_free);
	self->root = entry_new(NULL, NULL);
	reindex(self);

	system_path = find_system_menu();
	if (system_path != NULL) {
		g_autoptr(GError) local = NULL;

		if (gowl_menu_load_file(self, system_path, TRUE, &local))
			loaded = TRUE;
		else
			g_warning("menu: %s", local->message);
	}

	user_path = user_menu_path();
	if (g_file_test(user_path, G_FILE_TEST_EXISTS)) {
		g_autoptr(GError) local = NULL;

		/*
		 * A user file that will not parse leaves the shipped tree
		 * standing.  The alternative -- an empty menu because of one
		 * missing colon -- takes away the surface you would use to
		 * find out what went wrong.
		 */
		if (gowl_menu_load_file(self, user_path, TRUE, &local))
			loaded = TRUE;
		else
			g_warning("menu: %s: %s", user_path, local->message);
	}

	if (!loaded) {
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
		            "no menu.yaml found in %s or %s",
		            GOWL_DATADIR "/gowl", user_path);
		return FALSE;
	}
	return TRUE;
}

/* ────────────────────────────────────────────────────────────────────
 * Lookup
 * ──────────────────────────────────────────────────────────────────── */

static GowlMenuEntry *
lookup(GowlMenu *self, const gchar *route)
{
	if (route == NULL || *route == '\0'
	    || g_ascii_strcasecmp(route, "root") == 0)
		return self->root;
	return g_hash_table_lookup(self->by_route, route);
}

gchar *
gowl_menu_resolve(GowlMenu *self, const gchar *route)
{
	g_autofree gchar *norm = NULL;
	const gchar *hit;

	g_return_val_if_fail(GOWL_IS_MENU(self), g_strdup("root"));

	if (route == NULL || *route == '\0'
	    || g_ascii_strcasecmp(route, "root") == 0
	    || g_ascii_strcasecmp(route, "menu") == 0)
		return g_strdup("root");

	/* An exact route beats every alias, so a submenu can never be
	 * shadowed by something that happens to share its name. */
	if (g_hash_table_contains(self->by_route, route))
		return g_strdup(route);

	norm = route_normalise(route);
	if (g_hash_table_contains(self->by_route, norm))
		return g_steal_pointer(&norm);

	hit = g_hash_table_lookup(self->by_alias, norm);
	if (hit != NULL)
		return g_strdup(hit);

	/* Unknown: hand it back unchanged.  The caller then reports what
	 * was actually asked for rather than silently opening the root. */
	return g_strdup(route);
}

gboolean
gowl_menu_has_route(GowlMenu *self, const gchar *route)
{
	g_return_val_if_fail(GOWL_IS_MENU(self), FALSE);
	return lookup(self, route) != NULL;
}

guint
gowl_menu_n_entries(GowlMenu *self)
{
	g_return_val_if_fail(GOWL_IS_MENU(self), 0);
	return g_hash_table_size(self->by_route);
}

gboolean
gowl_menu_is_submenu(GowlMenu *self, const gchar *route)
{
	GowlMenuEntry *e;

	g_return_val_if_fail(GOWL_IS_MENU(self), FALSE);
	e = lookup(self, route);
	if (e == NULL)
		return FALSE;
	/*
	 * The root is a list by definition, even an empty one.  Answering
	 * FALSE for it sends the caller down the "this is an action" path,
	 * where an entry with no action does nothing and says nothing --
	 * so a menu whose file could not be found is indistinguishable
	 * from a key that is not bound.
	 */
	if (e == self->root)
		return TRUE;
	return e->children->len > 0 || e->provider != NULL || e->target != NULL;
}

gchar *
gowl_menu_get_title(GowlMenu *self, const gchar *route)
{
	GowlMenuEntry *e;

	g_return_val_if_fail(GOWL_IS_MENU(self), g_strdup("Menu"));
	e = lookup(self, route);
	if (e == NULL || e == self->root)
		return g_strdup("Menu");
	if (e->title != NULL)
		return g_strdup(e->title);
	return g_strdup(e->label != NULL ? e->label : e->id);
}

gchar *
gowl_menu_get_parent(GowlMenu *self, const gchar *route)
{
	const gchar *dot;

	g_return_val_if_fail(GOWL_IS_MENU(self), NULL);
	if (route == NULL || *route == '\0'
	    || g_ascii_strcasecmp(route, "root") == 0)
		return NULL;
	dot = strrchr(route, '.');
	if (dot == NULL)
		return g_strdup("root");
	return g_strndup(route, (gsize)(dot - route));
}

/* ────────────────────────────────────────────────────────────────────
 * Guard evaluation
 * ──────────────────────────────────────────────────────────────────── */

/*
 * An IPC reply reduced to yes or no.
 *
 * Replies are `OK <something>' for a command and a bare word for a
 * query, so the `OK ' is stripped before comparing -- otherwise every
 * `is:' in the tree would have to carry it, which is a detail of the
 * transport leaking into the menu file.
 */
static gboolean
ipc_reply_matches(const gchar *reply, const gchar *want)
{
	const gchar *body;
	g_autofree gchar *trimmed = NULL;

	if (reply == NULL)
		return FALSE;
	trimmed = g_strdup(reply);
	g_strstrip(trimmed);
	if (g_str_has_prefix(trimmed, "ERROR"))
		return FALSE;

	body = trimmed;
	if (g_str_has_prefix(body, "OK "))
		body += 3;
	else if (g_strcmp0(body, "OK") == 0)
		body += 2;

	if (want != NULL)
		return g_ascii_strcasecmp(body, want) == 0;

	return *body != '\0'
	       && g_ascii_strcasecmp(body, "no") != 0
	       && g_ascii_strcasecmp(body, "off") != 0
	       && g_ascii_strcasecmp(body, "false") != 0
	       && g_ascii_strcasecmp(body, "none") != 0
	       && g_strcmp0(body, "0") != 0;
}

static gboolean
guard_eval(const GowlMenuGuard *g, GowlCompositor *comp)
{
	gboolean answer = TRUE;

	switch (g->kind) {
	case GOWL_MENU_GUARD_NONE:
		return TRUE;
	case GOWL_MENU_GUARD_CONST:
		answer = g->truth;
		break;
	case GOWL_MENU_GUARD_EXISTS: {
		g_autofree gchar *found = g->key != NULL
			? g_find_program_in_path(g->key) : NULL;

		answer = found != NULL;
		break;
	}
	case GOWL_MENU_GUARD_FILE: {
		g_autofree gchar *path = NULL;

		if (g->key == NULL)
			return FALSE;
		if (g_str_has_prefix(g->key, "~/"))
			path = g_build_filename(g_get_home_dir(), g->key + 2, NULL);
		else
			path = g_strdup(g->key);
		answer = g_file_test(path, G_FILE_TEST_EXISTS);
		break;
	}
	case GOWL_MENU_GUARD_MODULE: {
		GowlModuleManager *mgr;

		if (comp == NULL || g->key == NULL)
			return FALSE;
		mgr = gowl_compositor_get_module_manager(comp);
		answer = mgr != NULL
		         && gowl_module_manager_find_module(mgr, g->key) != NULL;
		break;
	}
	case GOWL_MENU_GUARD_EMBEDDER:
		answer = comp != NULL && comp->custom_action_func != NULL;
		break;
	case GOWL_MENU_GUARD_IPC: {
		g_autofree gchar *reply = NULL;

		if (comp == NULL || g->key == NULL)
			return FALSE;
		reply = gowl_compositor_ipc_command(comp, g->key);
		answer = ipc_reply_matches(reply, g->is);
		break;
	}
	default:
		return TRUE;
	}

	return g->negate ? !answer : answer;
}

/* ────────────────────────────────────────────────────────────────────
 * Providers
 * ──────────────────────────────────────────────────────────────────── */

static void
provider_add(GPtrArray *out, const gchar *id, const gchar *icon,
             const gchar *label, const gchar *detail, const gchar *value,
             gboolean checked, GowlAction action, gchar *arg)
{
	GowlMenuProviderRow *r = g_new0(GowlMenuProviderRow, 1);

	r->id      = slugify(id != NULL ? id : label);
	r->icon    = g_strdup(icon);
	r->label   = g_strdup(label);
	r->detail  = g_strdup(detail);
	r->value   = g_strdup(value);
	r->checked = checked;
	r->action  = action;
	r->arg     = arg;                    /* transfer full */
	g_ptr_array_add(out, r);
}

/*
 * The launcher.
 *
 * Desktop entries, which is the only list of "programs on this machine"
 * that is both complete and curated -- a $PATH scan finds six hundred
 * things nobody launches by name.  The command line has its field codes
 * stripped: %U and friends are placeholders for files being opened,
 * which a menu never has.
 */
static gchar *
strip_field_codes(const gchar *cmdline)
{
	GString *out;
	const gchar *p;

	if (cmdline == NULL)
		return NULL;
	out = g_string_new(NULL);
	for (p = cmdline; *p != '\0'; p++) {
		if (p[0] == '%' && p[1] != '\0') {
			if (p[1] == '%') {
				g_string_append_c(out, '%');
				p++;
				continue;
			}
			if (g_ascii_isalpha(p[1])) {
				p++;
				continue;
			}
		}
		g_string_append_c(out, *p);
	}
	g_strstrip(out->str);
	g_string_set_size(out, strlen(out->str));
	return g_string_free(out, FALSE);
}

static gint
app_row_compare(gconstpointer a, gconstpointer b)
{
	const GowlMenuProviderRow *ra = *(GowlMenuProviderRow * const *)a;
	const GowlMenuProviderRow *rb = *(GowlMenuProviderRow * const *)b;

	return g_utf8_collate(ra->label != NULL ? ra->label : "",
	                      rb->label != NULL ? rb->label : "");
}

static void
provider_apps(GPtrArray *out)
{
	GList *apps, *l;

	apps = g_app_info_get_all();
	for (l = apps; l != NULL; l = l->next) {
		GAppInfo *info = l->data;
		g_autofree gchar *cmd = NULL;
		GIcon *icon;

		if (!g_app_info_should_show(info))
			continue;
		cmd = strip_field_codes(g_app_info_get_commandline(info));
		if (cmd == NULL || *cmd == '\0')
			continue;
		icon = g_app_info_get_icon(info);
		/*
		 * The NAME, not the pixels.  g_icon_to_string() gives the
		 * themed name a desktop entry's `Icon=' carries, or an
		 * absolute path when it carries one of those -- and both are
		 * what an icon-theme lookup takes.  Loading it here would
		 * put an image decoder in the model and decode 180 icons to
		 * draw fourteen.
		 */
		provider_add(out, g_app_info_get_id(info), "\xf3\xb0\x80\xbb",
		             g_app_info_get_display_name(info),
		             g_app_info_get_description(info), NULL, FALSE,
		             GOWL_ACTION_SPAWN, g_steal_pointer(&cmd));
		if (icon != NULL) {
			GowlMenuProviderRow *row =
				g_ptr_array_index(out, out->len - 1);

			row->icon_name = g_icon_to_string(icon);
		}
	}
	g_list_free_full(apps, g_object_unref);
	g_ptr_array_sort(out, app_row_compare);
}

static void
provider_windows(GPtrArray *out, GowlCompositor *comp)
{
	GList *l;
	GowlClient *focused;

	if (comp == NULL)
		return;
	focused = gowl_compositor_get_focused_client(comp);
	/* Borrowed: the compositor owns both the list and the clients, so
	 * nothing here is freed. */
	for (l = gowl_compositor_get_clients(comp); l != NULL; l = l->next) {
		GowlClient *c = l->data;
		const gchar *title;
		const gchar *app_id;
		g_autofree gchar *id = NULL;

		if (c == NULL)
			continue;
		title  = gowl_client_get_title(c);
		app_id = gowl_client_get_app_id(c);
		id     = g_strdup_printf("%u", gowl_client_get_id(c));
		provider_add(out, id, "\xf3\xb0\xa3\x86",
		             title != NULL && *title != '\0'
		                 ? title
		                 : (app_id != NULL ? app_id : "window"),
		             app_id, NULL, c == focused,
		             GOWL_ACTION_IPC_COMMAND,
		             g_strdup_printf("focus %s", id));
	}
}

static void
provider_layouts(GPtrArray *out, GowlCompositor *comp)
{
	GList *names, *l;
	GowlLayoutEntry *current;
	const gchar *now = NULL;

	if (comp == NULL)
		return;
	current = gowl_layout_get(comp, NULL);
	if (current != NULL)
		now = current->name;

	names = gowl_layout_list(comp);
	for (l = names; l != NULL; l = l->next) {
		const gchar *name = l->data;

		provider_add(out, name, "\xf3\xb1\x82\xac", name, NULL, NULL,
		             g_strcmp0(name, now) == 0,
		             GOWL_ACTION_SET_LAYOUT, g_strdup(name));
	}
	g_list_free(names);
}

static void
provider_tags(GPtrArray *out, GowlCompositor *comp)
{
	GowlMonitor *mon;
	guint32 active = 0;
	gint i;

	if (comp == NULL)
		return;
	mon = gowl_compositor_get_selected_monitor(comp);
	if (mon != NULL)
		active = gowl_monitor_get_tags(mon);

	for (i = 1; i <= 9; i++) {
		g_autofree gchar *label = g_strdup_printf("Tag %d", i);
		g_autofree gchar *id = g_strdup_printf("%d", i);
		guint32 mask = 1u << (i - 1);

		provider_add(out, id, "\xf3\xb0\x93\xb9", label, NULL, NULL,
		             (active & mask) != 0, GOWL_ACTION_TAG_VIEW,
		             g_strdup_printf("%u", mask));
	}
}

static void
provider_backdrops(GPtrArray *out, GowlCompositor *comp)
{
	GEnumClass *klass;
	const gchar *now = NULL;
	guint i;

	if (comp != NULL)
		now = gowl_config_backdrop_style_name(
			gowl_compositor_get_backdrop_style(comp));

	klass = (GEnumClass *)g_type_class_ref(GOWL_TYPE_BACKDROP_STYLE);
	for (i = 0; i < klass->n_values; i++) {
		const gchar *nick = klass->values[i].value_nick;

		provider_add(out, nick, "\xef\x80\xbe", nick, NULL, NULL,
		             g_strcmp0(nick, now) == 0,
		             GOWL_ACTION_IPC_COMMAND,
		             g_strdup_printf("backdrop %s", nick));
	}
	g_type_class_unref(klass);
}

static void
provider_keybinds(GPtrArray *out, GowlCompositor *comp)
{
	GArray *kbs;
	GowlConfig *config;
	guint i;

	if (comp == NULL)
		return;
	config = gowl_compositor_get_config(comp);
	if (config == NULL)
		return;
	kbs = gowl_config_get_keybinds(config);

	for (i = 0; kbs != NULL && i < kbs->len; i++) {
		GowlKeybindEntry *kb = &g_array_index(kbs, GowlKeybindEntry, i);
		g_autofree gchar *key = gowl_keybind_to_string(kb->modifiers,
		                                               kb->keysym);
		const gchar *label = kb->desc != NULL && *kb->desc != '\0'
			? kb->desc : (kb->arg != NULL ? kb->arg : "");

		/*
		 * The key is the VALUE rather than the label: this is a
		 * cheatsheet you read down the left, and a list of key
		 * combinations sorted by modifier is a list you cannot find
		 * anything in.
		 */
		provider_add(out, key, "\xef\x84\x9c", label, NULL, key, FALSE,
		             GOWL_ACTION_IPC_COMMAND,
		             g_strdup_printf("dispatch %s", key));
	}
}

static void
provider_tray(GPtrArray *out)
{
	g_autoptr(GPtrArray) items = NULL;
	guint i;

	items = gowl_tray_dup_items(gowl_tray_get_default());
	for (i = 0; items != NULL && i < items->len; i++) {
		GowlTrayItem *item = g_ptr_array_index(items, i);
		const gchar *label = item->title != NULL && *item->title != '\0'
			? item->title : item->id;

		provider_add(out, item->key, "\xf3\xb1\x8a\x94", label, item->status,
		             NULL, FALSE, GOWL_ACTION_IPC_COMMAND,
		             g_strdup_printf("tray activate %s", item->key));
	}
}

/*
 * Run a provider by name.
 *
 * An unknown name produces no rows rather than an error: the menu file
 * is data, a typo in it should cost that submenu and nothing else, and
 * the warning says which name was not recognised.
 */
static GPtrArray *
provider_run(GowlMenu *self, const gchar *name, GowlCompositor *comp)
{
	GPtrArray *out;
	gint64 ttl = provider_ttl(name);
	gint64 now = g_get_monotonic_time();

	if (ttl > 0 && self != NULL) {
		CachedRows *hit = g_hash_table_lookup(self->provider_cache, name);

		if (hit != NULL && now - hit->when < ttl)
			return g_ptr_array_ref(hit->rows);
	}

	out = g_ptr_array_new_with_free_func(provider_row_free);

	if (g_strcmp0(name, "apps") == 0)
		provider_apps(out);
	else if (g_strcmp0(name, "windows") == 0)
		provider_windows(out, comp);
	else if (g_strcmp0(name, "layouts") == 0)
		provider_layouts(out, comp);
	else if (g_strcmp0(name, "tags") == 0)
		provider_tags(out, comp);
	else if (g_strcmp0(name, "backdrops") == 0)
		provider_backdrops(out, comp);
	else if (g_strcmp0(name, "keybinds") == 0)
		provider_keybinds(out, comp);
	else if (g_strcmp0(name, "tray") == 0)
		provider_tray(out);
	else
		g_warning("menu: unknown provider `%s'", name);

	if (ttl > 0 && self != NULL) {
		CachedRows *entry = g_new0(CachedRows, 1);

		entry->rows = g_ptr_array_ref(out);
		entry->when = now;
		g_hash_table_replace(self->provider_cache, g_strdup(name), entry);
	}
	return out;
}

/* ────────────────────────────────────────────────────────────────────
 * Listing
 * ──────────────────────────────────────────────────────────────────── */

static gboolean entry_visible(GowlMenuEntry *e, GowlCompositor *comp);

static guint
count_visible_children(GowlMenuEntry *e, GowlCompositor *comp)
{
	guint i, n = 0;

	for (i = 0; i < e->children->len; i++) {
		GowlMenuEntry *c = g_ptr_array_index(e->children, i);

		if (entry_visible(c, comp))
			n++;
	}
	return n;
}

/*
 * Whether a row is offered at all.
 *
 * Its own `when:' first, and then the rule that a submenu whose every
 * child is hidden goes with them.  Without that second half a machine
 * with no laptop hardware still has a Hardware submenu, and entering it
 * finds an empty list -- which reads as a broken menu rather than as a
 * machine that has none of those things.
 *
 * A PROVIDER-backed submenu stays regardless, because its rows are made
 * when it is opened: "no windows are open" is a true and useful answer,
 * and deciding it in advance would mean running every provider on every
 * open of every level.
 */
static gboolean
entry_visible(GowlMenuEntry *e, GowlCompositor *comp)
{
	if (!guard_eval(&e->when, comp))
		return FALSE;
	if (e->children->len == 0 || e->provider != NULL || e->has_action)
		return TRUE;
	return count_visible_children(e, comp) > 0;
}

static GowlMenuRow *
row_from_entry(GowlMenuEntry *e, GowlCompositor *comp, const gchar *detail)
{
	GowlMenuRow *row = g_new0(GowlMenuRow, 1);

	row->route    = g_strdup(e->route);
	row->icon     = g_strdup(e->icon);
	row->label    = g_strdup(e->label != NULL ? e->label : e->id);
	row->detail   = g_strdup(detail != NULL ? detail : e->detail);
	row->submenu  = e->children->len > 0 || e->provider != NULL
	                || e->target != NULL;
	row->runnable = e->has_action;
	/*
	 * No guard means NOT ticked, where no `when:' guard means visible.
	 * The asymmetry is the whole point: absence of a rule is permission
	 * to show a row and is not a claim about what it is set to.  Read
	 * the other way round, every row in the menu wears a tick.
	 */
	row->checked  = e->checked.kind != GOWL_MENU_GUARD_NONE
	                && guard_eval(&e->checked, comp);
	row->disabled = e->disabled.kind != GOWL_MENU_GUARD_NONE
	                && guard_eval(&e->disabled, comp);
	row->children = count_visible_children(e, comp);
	/*
	 * A disabled row reads as "you already have this", which is the
	 * same thing the tick means everywhere else in the tree -- so it
	 * gets the same tick rather than a second convention.
	 */
	if (row->disabled)
		row->checked = TRUE;
	return row;
}

static GowlMenuRow *
row_from_provider(GowlMenuEntry *parent, GowlMenuProviderRow *p)
{
	GowlMenuRow *row = g_new0(GowlMenuRow, 1);

	row->route     = g_strdup_printf("%s.%s", parent->route, p->id);
	row->icon      = g_strdup(p->icon);
	row->icon_name = g_strdup(p->icon_name);
	row->label   = g_strdup(p->label);
	row->detail  = g_strdup(p->detail);
	row->value   = g_strdup(p->value);
	row->checked  = p->checked;
	row->runnable = TRUE;
	return row;
}

GPtrArray *
gowl_menu_list(GowlMenu *self, GowlCompositor *comp, const gchar *route)
{
	GowlMenuEntry *e;
	GPtrArray *out;
	guint i;

	g_return_val_if_fail(GOWL_IS_MENU(self), NULL);

	out = g_ptr_array_new_with_free_func((GDestroyNotify)gowl_menu_row_free);
	e = lookup(self, route);
	if (e == NULL)
		return out;

	/* A link lists what it points at, so a row can appear in two places
	 * without its children being declared twice. */
	if (e->target != NULL && e->children->len == 0 && e->provider == NULL) {
		GowlMenuEntry *target = lookup(self, e->target);

		if (target != NULL && target != e)
			e = target;
	}

	for (i = 0; i < e->children->len; i++) {
		GowlMenuEntry *c = g_ptr_array_index(e->children, i);

		if (!entry_visible(c, comp))
			continue;
		g_ptr_array_add(out, row_from_entry(c, comp, NULL));
	}

	if (e->provider != NULL) {
		g_autoptr(GPtrArray) rows = provider_run(self, e->provider, comp);

		for (i = 0; rows != NULL && i < rows->len; i++) {
			g_ptr_array_add(out, row_from_provider(
				e, g_ptr_array_index(rows, i)));
		}
	}
	return out;
}

/* ────────────────────────────────────────────────────────────────────
 * Search
 * ──────────────────────────────────────────────────────────────────── */

typedef struct {
	GowlMenuRow *row;
	gint         score;
	guint        order;
} SearchHit;

static GowlMenuRow *row_from_provider(GowlMenuEntry *parent,
                                      GowlMenuProviderRow *p);

/*
 * How well one row answers one query.
 *
 * Lower is better.  The tiers are deliberately coarse -- exact, prefix,
 * substring, description -- because a finer ranking over a list this
 * small is a ranking nobody can predict, and a menu whose first row
 * moves for reasons you cannot see is worse than one that is merely
 * alphabetical.
 */
static gint
search_score(const gchar *label, const gchar *detail, const gchar *path,
             const gchar *needle)
{
	g_autofree gchar *l = g_utf8_casefold(label != NULL ? label : "", -1);
	g_autofree gchar *d = g_utf8_casefold(detail != NULL ? detail : "", -1);
	g_autofree gchar *p = g_utf8_casefold(path != NULL ? path : "", -1);

	if (g_strcmp0(l, needle) == 0)
		return 0;
	if (g_str_has_prefix(l, needle))
		return 10;
	if (strstr(l, needle) != NULL)
		return 30;
	if (*d != '\0' && strstr(d, needle) != NULL)
		return 50;
	if (strstr(p, needle) != NULL)
		return 70;
	return -1;
}

static gint
search_compare(gconstpointer a, gconstpointer b)
{
	const SearchHit *ha = a;
	const SearchHit *hb = b;

	if (ha->score != hb->score)
		return ha->score - hb->score;
	return (gint)ha->order - (gint)hb->order;
}

/* The breadcrumb a search result carries, so a row found from the root
 * still says where it lives. */
static gchar *
path_text(GowlMenu *self, GowlMenuEntry *e)
{
	GString *out = g_string_new(NULL);
	g_autofree gchar *parent_route = gowl_menu_get_parent(self, e->route);
	GowlMenuEntry *p = parent_route != NULL ? lookup(self, parent_route) : NULL;
	GPtrArray *stack = g_ptr_array_new();
	guint i;

	while (p != NULL && p != self->root) {
		g_autofree gchar *up = NULL;

		g_ptr_array_add(stack, p);
		up = gowl_menu_get_parent(self, p->route);
		p  = up != NULL ? lookup(self, up) : NULL;
	}
	for (i = stack->len; i > 0; i--) {
		GowlMenuEntry *node = g_ptr_array_index(stack, i - 1);

		if (out->len > 0)
			g_string_append(out, " / ");
		g_string_append(out, node->label != NULL ? node->label : node->id);
	}
	g_ptr_array_free(stack, TRUE);
	return g_string_free(out, FALSE);
}

static void
search_walk(GowlMenu *self, GowlMenuEntry *e, GowlCompositor *comp,
            const gchar *needle, GArray *hits, guint *order)
{
	guint i;

	for (i = 0; i < e->children->len; i++) {
		GowlMenuEntry *c = g_ptr_array_index(e->children, i);
		g_autofree gchar *path = NULL;
		gint score;

		if (!entry_visible(c, comp))
			continue;

		path  = path_text(self, c);
		score = search_score(c->label != NULL ? c->label : c->id,
		                     c->detail, path, needle);
		if (score >= 0) {
			SearchHit hit;

			/* A submenu is a slightly worse answer than a leaf
			 * that matched as well: choosing it costs another
			 * keystroke and the leaf is what was asked for. */
			hit.row   = row_from_entry(c, comp, path);
			hit.score = score * 1000
			            + (hit.row->submenu ? 2 : 0)
			            + (gint)(*order % 1000);
			hit.order = *order;
			g_array_append_val(hits, hit);
		}
		(*order)++;

		/*
		 * And what the submenu would MAKE if it were opened.
		 *
		 * Without this, typing finds every row somebody wrote in
		 * menu.yaml and none of the two hundred applications on the
		 * machine -- which is the half of the list people actually
		 * search for.  The cost is the providers themselves; the
		 * expensive one is cached (provider_ttl), which is what
		 * makes this affordable on every keystroke.
		 */
		if (c->provider != NULL) {
			g_autoptr(GPtrArray) made =
				provider_run(self, c->provider, comp);
			/* The path TO the provider's submenu, which is its
			 * own ancestors plus itself -- path_text() gives
			 * only the ancestors, and a row under a top-level
			 * submenu would otherwise read " / Apps". */
			g_autofree gchar *above = path_text(self, c);
			g_autofree gchar *child_path = NULL;
			guint k;

			child_path = (above != NULL && *above != '\0')
				? g_strdup_printf("%s / %s", above,
				                  c->label != NULL ? c->label : c->id)
				: g_strdup(c->label != NULL ? c->label : c->id);

			for (k = 0; made != NULL && k < made->len; k++) {
				GowlMenuProviderRow *pr =
					g_ptr_array_index(made, k);
				SearchHit hit;
				gint s2;

				s2 = search_score(pr->label, pr->detail,
				                  child_path, needle);
				if (s2 < 0)
					continue;
				hit.row   = row_from_provider(c, pr);
				g_free(hit.row->detail);
				hit.row->detail = g_strdup(child_path);
				/*
				 * A provider row loses every tie to a
				 * declared one that matched as well.  An
				 * application called `Settings' must not
				 * take the route the Setup menu is reached
				 * by, which is the mistake omarchy documents
				 * having made.
				 */
				hit.score = s2 * 1000 + 500
				            + (gint)(*order % 500);
				hit.order = *order;
				g_array_append_val(hits, hit);
				(*order)++;
			}
		}

		search_walk(self, c, comp, needle, hits, order);
	}
}

GPtrArray *
gowl_menu_search(GowlMenu *self, GowlCompositor *comp, const gchar *text)
{
	g_autoptr(GArray) hits = NULL;
	g_autofree gchar *needle = NULL;
	GPtrArray *out;
	guint order = 0;
	guint i;

	g_return_val_if_fail(GOWL_IS_MENU(self), NULL);

	out = g_ptr_array_new_with_free_func((GDestroyNotify)gowl_menu_row_free);
	if (text == NULL || *text == '\0')
		return out;

	needle = g_utf8_casefold(text, -1);
	g_strstrip(needle);
	if (*needle == '\0')
		return out;

	hits = g_array_new(FALSE, FALSE, sizeof(SearchHit));
	search_walk(self, self->root, comp, needle, hits, &order);
	g_array_sort(hits, search_compare);

	for (i = 0; i < hits->len; i++)
		g_ptr_array_add(out, g_array_index(hits, SearchHit, i).row);
	return out;
}

/* ────────────────────────────────────────────────────────────────────
 * Activation
 * ──────────────────────────────────────────────────────────────────── */

static gboolean
run_entry_action(GowlCompositor *comp, GowlAction action, const gchar *arg)
{
	GowlKeybindEntry kb;

	if (comp == NULL)
		return FALSE;

	memset(&kb, 0, sizeof kb);
	kb.action = action;
	kb.arg    = (gchar *)(arg != NULL && *arg != '\0' ? arg : NULL);
	return gowl_compositor_run_keybind_entry(comp, &kb);
}

/*
 * A provider row, acted on by re-running its provider.
 *
 * The rows are not stored, so the only way to know what `apps.firefox'
 * means is to ask the provider again -- which is also the only CORRECT
 * way, since the list may have changed between being drawn and being
 * chosen, and acting on a stale row is how a menu closes a window that
 * is no longer the one under the cursor.
 */
static GowlMenuResult
activate_provider_row(GowlMenu *self, GowlCompositor *comp,
                      GowlMenuEntry *parent, const gchar *leaf)
{
	g_autoptr(GPtrArray) rows = NULL;
	guint i;

	rows = provider_run(self, parent->provider, comp);
	for (i = 0; rows != NULL && i < rows->len; i++) {
		GowlMenuProviderRow *r = g_ptr_array_index(rows, i);
		gboolean keep_open;

		if (g_strcmp0(r->id, leaf) != 0)
			continue;
		/* Read before running: see the note in gowl_menu_activate(). */
		keep_open = parent->keep_open;
		if (!run_entry_action(comp, r->action, r->arg))
			return GOWL_MENU_RESULT_NONE;
		return keep_open ? GOWL_MENU_RESULT_RAN_OPEN
		                 : GOWL_MENU_RESULT_RAN;
	}
	return GOWL_MENU_RESULT_NONE;
}

GowlMenuResult
gowl_menu_activate(GowlMenu *self, GowlCompositor *comp, const gchar *route,
                   gchar **out_route)
{
	GowlMenuEntry *e;

	g_return_val_if_fail(GOWL_IS_MENU(self), GOWL_MENU_RESULT_NONE);

	if (out_route != NULL)
		*out_route = NULL;

	e = lookup(self, route);
	if (e == NULL) {
		/* Not in the tree: it may be a row a provider made up, whose
		 * parent is. */
		g_autofree gchar *parent_route = gowl_menu_get_parent(self, route);
		GowlMenuEntry *parent = parent_route != NULL
			? lookup(self, parent_route) : NULL;
		const gchar *leaf = strrchr(route != NULL ? route : "", '.');

		if (parent != NULL && parent->provider != NULL && leaf != NULL)
			return activate_provider_row(self, comp, parent, leaf + 1);
		return GOWL_MENU_RESULT_NONE;
	}

	/* A link opens what it points at. */
	if (e->target != NULL && !e->has_action) {
		if (out_route != NULL)
			*out_route = g_strdup(e->target);
		return GOWL_MENU_RESULT_OPEN;
	}

	if (e->children->len > 0 || e->provider != NULL) {
		if (out_route != NULL)
			*out_route = g_strdup(e->route != NULL ? e->route : "root");
		return GOWL_MENU_RESULT_OPEN;
	}

	if (!e->has_action)
		return GOWL_MENU_RESULT_NONE;
	if (e->disabled.kind != GOWL_MENU_GUARD_NONE
	    && guard_eval(&e->disabled, comp))
		return GOWL_MENU_RESULT_NONE;

	/*
	 * EVERYTHING IS READ OUT OF THE ENTRY BEFORE ANYTHING RUNS, and
	 * the argument is copied.
	 *
	 * What a row does can reload this tree -- `Reload this menu' is a
	 * shipped row, and any `spawn:' that writes menu.yaml has the same
	 * effect a moment later -- and a reload frees every entry.  Reading
	 * `e->keep_open' after the call, or handing the dispatcher a
	 * pointer into `e->arg' while it runs, is then a read of freed
	 * memory in the compositor process: under `cmacs --gowl' that is
	 * the whole desktop.
	 */
	{
		GowlAction action = e->action;
		gboolean keep_open = e->keep_open;
		g_autofree gchar *arg = g_strdup(e->arg);

		if (!run_entry_action(comp, action, arg))
			return GOWL_MENU_RESULT_NONE;
		return keep_open ? GOWL_MENU_RESULT_RAN_OPEN
		                 : GOWL_MENU_RESULT_RAN;
	}
}
