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
 * gowl-floatdesk-apps.c - windows to applications.  See the header.
 */

#include <string.h>
#include <gio/gio.h>
#include <gio/gdesktopappinfo.h>

#include "gowl-floatdesk-apps.h"

struct _GowlFloatdeskCatalog {
	/* id -> GowlFloatdeskApp*, owning */
	GHashTable *by_id;
};

/* --- Lifetime ---------------------------------------------------- */

static void
app_free(gpointer data)
{
	GowlFloatdeskApp *app = data;

	if (app == NULL)
		return;
	g_free(app->id);
	g_free(app->name);
	g_free(app->icon);
	g_free(app->exec);
	g_free(app->wm_class);
	g_free(app);
}

GowlFloatdeskCatalog *
gowl_floatdesk_catalog_new(void)
{
	GowlFloatdeskCatalog *self;

	self = g_new0(GowlFloatdeskCatalog, 1);
	self->by_id = g_hash_table_new_full(g_str_hash, g_str_equal, NULL,
	                                    app_free);
	return self;
}

void
gowl_floatdesk_catalog_free(GowlFloatdeskCatalog *self)
{
	if (self == NULL)
		return;
	g_hash_table_unref(self->by_id);
	g_free(self);
}

/*
 * bare_id:
 * @id: an entry id
 *
 * Returns: (transfer full): @id without a trailing `.desktop'
 */
static gchar *
bare_id(const gchar *id)
{
	if (g_str_has_suffix(id, ".desktop"))
		return g_strndup(id, strlen(id) - strlen(".desktop"));
	return g_strdup(id);
}

/* --- Field codes ------------------------------------------------- */

gchar *
gowl_floatdesk_strip_field_codes(const gchar *exec)
{
	GString     *out;
	const gchar *p;

	if (exec == NULL)
		return NULL;

	out = g_string_new(NULL);
	for (p = exec; *p != '\0'; p++) {
		if (*p != '%') {
			g_string_append_c(out, *p);
			continue;
		}
		/* "%%" is a literal percent; every other code is dropped.
		 * A trailing lone `%' is dropped with it. */
		if (p[1] == '%') {
			g_string_append_c(out, '%');
			p++;
		} else if (p[1] != '\0') {
			p++;
		}
	}
	g_strstrip(out->str);
	out->len = strlen(out->str);
	return g_string_free(out, FALSE);
}

/* --- Filling ----------------------------------------------------- */

void
gowl_floatdesk_catalog_add(
	GowlFloatdeskCatalog *self,
	const gchar          *id,
	const gchar          *name,
	const gchar          *icon,
	const gchar          *exec,
	const gchar          *wm_class,
	gboolean              terminal,
	gboolean              hidden
){
	GowlFloatdeskApp *app;

	g_return_if_fail(self != NULL);
	g_return_if_fail(id != NULL && *id != '\0');

	app = g_new0(GowlFloatdeskApp, 1);
	app->id = bare_id(id);
	app->name = g_strdup(name != NULL && *name != '\0' ? name : app->id);
	app->icon = g_strdup(icon);
	app->exec = gowl_floatdesk_strip_field_codes(exec);
	app->wm_class = (wm_class != NULL && *wm_class != '\0')
		? g_strdup(wm_class) : NULL;
	app->terminal = terminal;
	app->hidden = hidden;
	/* The key is the app's own id string, so replacing frees the old
	 * key with the old value. */
	g_hash_table_replace(self->by_id, app->id, app);
}

guint
gowl_floatdesk_catalog_load_system(GowlFloatdeskCatalog *self)
{
	GList *apps, *l;
	guint  added = 0;

	g_return_val_if_fail(self != NULL, 0);

	apps = g_app_info_get_all();
	for (l = apps; l != NULL; l = l->next) {
		GAppInfo         *info = l->data;
		GDesktopAppInfo  *desk;
		g_autofree gchar *icon = NULL;
		const gchar      *id;
		GIcon            *gicon;

		id = g_app_info_get_id(info);
		if (id == NULL || !G_IS_DESKTOP_APP_INFO(info))
			continue;
		desk = G_DESKTOP_APP_INFO(info);
		gicon = g_app_info_get_icon(info);
		/* The icon's NAME (or path), not its pixels: decoding every
		 * installed icon to draw the dozen in the dock would be a
		 * startup cost for nothing. */
		if (gicon != NULL)
			icon = g_icon_to_string(gicon);
		gowl_floatdesk_catalog_add(self, id,
			g_app_info_get_display_name(info), icon,
			g_app_info_get_commandline(info),
			g_desktop_app_info_get_startup_wm_class(desk),
			g_desktop_app_info_get_boolean(desk, "Terminal"),
			!g_app_info_should_show(info));
		added++;
	}
	g_list_free_full(apps, g_object_unref);
	return added;
}

/* --- Resolving --------------------------------------------------- */

const GowlFloatdeskApp *
gowl_floatdesk_catalog_lookup(
	const GowlFloatdeskCatalog *self,
	const gchar                *id
){
	g_autofree gchar *bare = NULL;

	g_return_val_if_fail(self != NULL, NULL);

	if (id == NULL || *id == '\0')
		return NULL;
	bare = bare_id(id);
	return g_hash_table_lookup(self->by_id, bare);
}

/*
 * last_part:
 * @id: a dotted id
 *
 * Returns: (transfer none): what follows the last dot, or @id itself
 */
static const gchar *
last_part(const gchar *id)
{
	const gchar *dot = strrchr(id, '.');

	return dot != NULL && dot[1] != '\0' ? dot + 1 : id;
}

/*
 * resolve_plain:
 * @self: a catalog
 * @app_id: an app_id with no `tui.' prefix
 *
 * The id, case and class matches.  The passes are separate rather than
 * one loop scoring each entry, so an exact id always beats a class match
 * on some other entry, however the hash table happens to iterate.
 */
static const GowlFloatdeskApp *
resolve_plain(
	const GowlFloatdeskCatalog *self,
	const gchar                *app_id
){
	const GowlFloatdeskApp *found;
	GHashTableIter          iter;
	gpointer                value;
	const gchar            *tail;

	found = gowl_floatdesk_catalog_lookup(self, app_id);
	if (found != NULL)
		return found;

	g_hash_table_iter_init(&iter, self->by_id);
	while (g_hash_table_iter_next(&iter, NULL, &value)) {
		const GowlFloatdeskApp *app = value;

		if (g_ascii_strcasecmp(app->id, app_id) == 0)
			return app;
	}

	g_hash_table_iter_init(&iter, self->by_id);
	while (g_hash_table_iter_next(&iter, NULL, &value)) {
		const GowlFloatdeskApp *app = value;

		if (app->wm_class != NULL
		    && g_ascii_strcasecmp(app->wm_class, app_id) == 0)
			return app;
	}

	/* `org.gnome.Nautilus' for an entry `nautilus', and the reverse.
	 * Only an exact match of the parts: a substring test would make
	 * `code' claim `vscode-insiders'. */
	tail = last_part(app_id);
	g_hash_table_iter_init(&iter, self->by_id);
	while (g_hash_table_iter_next(&iter, NULL, &value)) {
		const GowlFloatdeskApp *app = value;

		if (g_ascii_strcasecmp(last_part(app->id), tail) == 0)
			return app;
	}
	return NULL;
}

const GowlFloatdeskApp *
gowl_floatdesk_catalog_resolve(
	const GowlFloatdeskCatalog *self,
	const gchar                *app_id
){
	g_return_val_if_fail(self != NULL, NULL);

	if (app_id == NULL || *app_id == '\0')
		return NULL;
	if (g_ascii_strncasecmp(app_id, GOWL_FLOATDESK_TUI_PREFIX,
	                        strlen(GOWL_FLOATDESK_TUI_PREFIX)) == 0
	    && app_id[strlen(GOWL_FLOATDESK_TUI_PREFIX)] != '\0')
		return resolve_plain(self,
		                     app_id + strlen(GOWL_FLOATDESK_TUI_PREFIX));
	return resolve_plain(self, app_id);
}

gchar *
gowl_floatdesk_catalog_key(
	const GowlFloatdeskCatalog *self,
	const gchar                *app_id
){
	const GowlFloatdeskApp *app;

	g_return_val_if_fail(self != NULL, NULL);

	app = gowl_floatdesk_catalog_resolve(self, app_id);
	if (app != NULL)
		return g_strdup(app->id);
	if (app_id != NULL && *app_id != '\0')
		return g_ascii_strdown(app_id, -1);
	return g_strdup("unknown");
}

static gint
app_by_name(
	gconstpointer a,
	gconstpointer b
){
	const GowlFloatdeskApp *x = *(const GowlFloatdeskApp * const *)a;
	const GowlFloatdeskApp *y = *(const GowlFloatdeskApp * const *)b;

	return g_utf8_collate(x->name, y->name);
}

GPtrArray *
gowl_floatdesk_catalog_list(const GowlFloatdeskCatalog *self)
{
	GPtrArray      *out;
	GHashTableIter  iter;
	gpointer        value;

	g_return_val_if_fail(self != NULL, NULL);

	out = g_ptr_array_new();
	g_hash_table_iter_init(&iter, self->by_id);
	while (g_hash_table_iter_next(&iter, NULL, &value)) {
		GowlFloatdeskApp *app = value;

		if (!app->hidden && app->exec != NULL && *app->exec != '\0')
			g_ptr_array_add(out, app);
	}
	g_ptr_array_sort(out, app_by_name);
	return out;
}

/* --- Launching in a terminal ------------------------------------- */

gchar **
gowl_floatdesk_terminal_argv(
	const gchar *terminal,
	const gchar *app_id,
	const gchar *command,
	GError     **error
){
	g_auto(GStrv)     cmd = NULL;
	g_auto(GStrv)     term = NULL;
	g_autofree gchar *base = NULL;
	GPtrArray        *argv;
	guint             i;

	g_return_val_if_fail(terminal != NULL, NULL);
	g_return_val_if_fail(command != NULL, NULL);

	if (!g_shell_parse_argv(command, NULL, &cmd, error))
		return NULL;
	/* The terminal setting may carry its own arguments ("foot -o
	 * font=..."): parse it too, and key the class flag off its
	 * program name. */
	if (!g_shell_parse_argv(terminal, NULL, &term, error))
		return NULL;
	base = g_path_get_basename(term[0]);

	argv = g_ptr_array_new();
	for (i = 0; term[i] != NULL; i++)
		g_ptr_array_add(argv, g_strdup(term[i]));

	if (app_id == NULL || *app_id == '\0') {
		g_ptr_array_add(argv, g_strdup("-e"));
	} else if (g_strcmp0(base, "foot") == 0
	           || g_strcmp0(base, "footclient") == 0) {
		/* foot takes the command as its trailing arguments. */
		g_ptr_array_add(argv, g_strdup_printf("--app-id=%s", app_id));
	} else if (g_strcmp0(base, "kitty") == 0) {
		g_ptr_array_add(argv, g_strdup("--class"));
		g_ptr_array_add(argv, g_strdup(app_id));
	} else if (g_strcmp0(base, "ghostty") == 0) {
		g_ptr_array_add(argv, g_strdup_printf("--class=%s", app_id));
		g_ptr_array_add(argv, g_strdup("-e"));
	} else if (g_strcmp0(base, "alacritty") == 0) {
		g_ptr_array_add(argv, g_strdup("--class"));
		g_ptr_array_add(argv, g_strdup(app_id));
		g_ptr_array_add(argv, g_strdup("-e"));
	} else if (g_strcmp0(base, "wezterm") == 0) {
		g_ptr_array_add(argv, g_strdup("start"));
		g_ptr_array_add(argv, g_strdup("--class"));
		g_ptr_array_add(argv, g_strdup(app_id));
		g_ptr_array_add(argv, g_strdup("--"));
	} else if (g_strcmp0(base, "st") == 0 || g_strcmp0(base, "gst") == 0) {
		g_ptr_array_add(argv, g_strdup("-c"));
		g_ptr_array_add(argv, g_strdup(app_id));
		g_ptr_array_add(argv, g_strdup("-e"));
	} else {
		/* Unknown: run it, lose the identity. */
		g_ptr_array_add(argv, g_strdup("-e"));
	}

	for (i = 0; cmd[i] != NULL; i++)
		g_ptr_array_add(argv, g_strdup(cmd[i]));
	g_ptr_array_add(argv, NULL);
	return (gchar **)g_ptr_array_free(argv, FALSE);
}

/* --- Click cycling ----------------------------------------------- */

gint
gowl_floatdesk_cycle_next(
	guint count,
	gint  focused
){
	if (count == 0)
		return -1;
	if (focused < 0 || (guint)focused >= count)
		return 0;
	return (gint)(((guint)focused + 1) % count);
}
