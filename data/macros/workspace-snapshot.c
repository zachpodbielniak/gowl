/*
 * workspace-snapshot.c - remember which tag each application's windows
 * are on, and put them back later.
 *
 * gowl - GObject Wayland Compositor.  Example macro.
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Run it:   gowl-msg macro-run workspace-snapshot save
 *           gowl-msg macro-run workspace-snapshot restore
 *           gowl-msg macro-run workspace-snapshot save ~/work.snapshot
 *
 * The file (default $XDG_STATE_HOME/gowl/workspace.snapshot) is a
 * GKeyFile: [tags] APP-ID=TAGMASK.  Restore moves every window of each
 * app-id to its saved tags; apps with several windows on different
 * tags are saved by their first window.
 */

#include <gowl/gowl.h>

G_MODULE_EXPORT const gchar *
gowl_macro_info(void)
{
	return "save|restore [FILE]: app-id -> tags placement";
}

static gchar *
snapshot_path(GowlMacroContext *ctx)
{
	const gchar *given = gowl_macro_get_arg(ctx, 1);

	if (given == NULL)
		return g_build_filename(g_get_user_state_dir(), "gowl",
		                        "workspace.snapshot", NULL);
	if (g_str_has_prefix(given, "~/"))
		return g_build_filename(g_get_home_dir(), given + 2, NULL);
	return g_strdup(given);
}

static gboolean
save(
	GowlMacroContext *ctx,
	const gchar      *path
){
	g_autoptr(GKeyFile) kf = g_key_file_new();
	g_autoptr(GError) error = NULL;
	g_autofree gchar *dir = NULL;
	g_autofree gchar *msg = NULL;
	GList *all;
	GList *l;
	guint n;

	n = 0;
	all = gowl_macro_list_clients(ctx, FALSE);
	for (l = all; l != NULL; l = l->next) {
		const gchar *app = gowl_client_get_app_id(l->data);

		if (app == NULL || *app == '\0'
		    || g_key_file_has_key(kf, "tags", app, NULL))
			continue;
		g_key_file_set_uint64(kf, "tags", app,
		                      gowl_client_get_tags(l->data));
		n++;
	}
	g_list_free(all);

	dir = g_path_get_dirname(path);
	g_mkdir_with_parents(dir, 0700);
	if (!g_key_file_save_to_file(kf, path, &error)) {
		gowl_macro_set_result(ctx, error->message);
		return FALSE;
	}
	msg = g_strdup_printf("saved %u app(s) to %s", n, path);
	gowl_macro_set_result(ctx, msg);
	return TRUE;
}

static gboolean
restore(
	GowlMacroContext *ctx,
	const gchar      *path
){
	g_autoptr(GKeyFile) kf = g_key_file_new();
	g_autoptr(GError) error = NULL;
	g_autofree gchar *msg = NULL;
	GList *all;
	GList *l;
	guint moved;

	if (!g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, &error)) {
		gowl_macro_set_result(ctx, error->message);
		return FALSE;
	}
	moved = 0;
	all = gowl_macro_list_clients(ctx, FALSE);
	for (l = all; l != NULL; l = l->next) {
		const gchar *app = gowl_client_get_app_id(l->data);
		guint64 tags;

		if (app == NULL || !g_key_file_has_key(kf, "tags", app, NULL))
			continue;
		tags = g_key_file_get_uint64(kf, "tags", app, NULL);
		if (tags != 0) {
			gowl_macro_move_client(ctx, l->data, NULL, (guint32)tags);
			moved++;
		}
	}
	g_list_free(all);
	msg = g_strdup_printf("restored %u window(s)", moved);
	gowl_macro_set_result(ctx, msg);
	return TRUE;
}

G_MODULE_EXPORT gboolean
gowl_macro_run(GowlMacroContext *ctx)
{
	const gchar *verb;
	g_autofree gchar *path = NULL;

	verb = gowl_macro_get_arg(ctx, 0);
	path = snapshot_path(ctx);
	if (g_strcmp0(verb, "save") == 0)
		return save(ctx, path);
	if (g_strcmp0(verb, "restore") == 0)
		return restore(ctx, path);
	gowl_macro_set_result(ctx, "usage: workspace-snapshot save|restore [FILE]");
	return FALSE;
}
