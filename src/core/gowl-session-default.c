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

#include "gowl-session-default.h"
#include "gowl-compositor.h"
#include "gowl-client.h"
#include "gowl-monitor.h"
#include "gowl-core-private.h"
#include "../boxed/gowl-geometry.h"
#include "../config/gowl-config.h"
#include "yaml-glib.h"

#include <stdio.h>
#include <string.h>

typedef struct {
	int _unused;
} GowlSessionDefaultPrivate;

static void gowl_session_default_iface_init(
	GowlSessionProviderInterface *iface);

G_DEFINE_TYPE_WITH_CODE(GowlSessionDefault,
                        gowl_session_default,
                        G_TYPE_OBJECT,
                        G_ADD_PRIVATE(GowlSessionDefault)
                        G_IMPLEMENT_INTERFACE(
                            GOWL_TYPE_SESSION_PROVIDER,
                            gowl_session_default_iface_init))

/* -----------------------------------------------------------
 * Save: iterate client list + monitors, emit YAML by hand.  The
 * schema is intentionally small and predictable so subclasses /
 * rule-providers can consume it directly.
 * ----------------------------------------------------------- */

static void
append_escaped_yaml_string(GString *out, const gchar *s)
{
	guint i;

	if (s == NULL) {
		g_string_append(out, "\"\"");
		return;
	}

	g_string_append_c(out, '"');
	for (i = 0; s[i] != '\0'; i++) {
		gchar c = s[i];
		if (c == '\\' || c == '"')
			g_string_append_c(out, '\\');
		g_string_append_c(out, c);
	}
	g_string_append_c(out, '"');
}

static gboolean
gowl_session_default_save_impl(GowlSessionProvider *provider,
                                gpointer             compositor_ptr,
                                GFile               *dest,
                                GError             **error)
{
	GowlCompositor  *compositor;
	g_autoptr(GString) yaml = NULL;
	GList           *clients, *l;
	GList           *monitors;
	guint            monitor_idx;

	(void)provider;

	if (!GOWL_IS_COMPOSITOR(compositor_ptr)) {
		g_set_error_literal(error, G_IO_ERROR,
		                    G_IO_ERROR_INVALID_ARGUMENT,
		                    "compositor argument is not a GowlCompositor");
		return FALSE;
	}

	compositor = (GowlCompositor *)compositor_ptr;
	yaml = g_string_new("# gowl session snapshot\n");

	/* Monitors */
	g_string_append(yaml, "monitors:\n");
	monitors = gowl_compositor_get_monitors(compositor);
	monitor_idx = 0;
	for (l = monitors; l != NULL; l = l->next, monitor_idx++) {
		GowlMonitor *m = GOWL_MONITOR(l->data);
		const gchar *name = gowl_monitor_get_name(m);
		g_string_append_printf(yaml, "  - index: %u\n", monitor_idx);
		g_string_append(yaml, "    name: ");
		append_escaped_yaml_string(yaml, name != NULL ? name : "");
		g_string_append_c(yaml, '\n');
		g_string_append_printf(yaml, "    mfact: %.3f\n",
		                       gowl_monitor_get_mfact(m));
		g_string_append_printf(yaml, "    nmaster: %d\n",
		                       gowl_monitor_get_nmaster(m));
	}

	/* Clients */
	g_string_append(yaml, "clients:\n");
	clients = gowl_compositor_get_clients(compositor);
	for (l = clients; l != NULL; l = l->next) {
		GowlClient  *c = GOWL_CLIENT(l->data);
		const gchar *app_id;
		const gchar *title;
		gint         gx, gy, gw, gh;

		app_id = gowl_client_get_app_id(c);
		title  = gowl_client_get_title(c);
		gowl_client_get_geometry(c, &gx, &gy, &gw, &gh);

		g_string_append(yaml, "  - app_id: ");
		append_escaped_yaml_string(yaml, app_id != NULL ? app_id : "");
		g_string_append_c(yaml, '\n');
		g_string_append(yaml, "    title: ");
		append_escaped_yaml_string(yaml, title != NULL ? title : "");
		g_string_append_c(yaml, '\n');
		g_string_append_printf(yaml, "    tags: %u\n",
		                       gowl_client_get_tags(c));
		g_string_append_printf(yaml, "    floating: %s\n",
		                       gowl_client_get_floating(c)
		                       ? "true" : "false");
		g_string_append_printf(yaml,
		                       "    geometry: [%d, %d, %d, %d]\n",
		                       gx, gy, gw, gh);
	}

	return g_file_replace_contents(dest, yaml->str, yaml->len,
	                                NULL, FALSE,
	                                G_FILE_CREATE_REPLACE_DESTINATION,
	                                NULL, NULL, error);
}

/* -----------------------------------------------------------
 * Load: parse YAML, apply per-monitor state, queue per-client
 * rules into the compositor's GowlConfig so that subsequently
 * mapped clients matching by app_id inherit tags + floating +
 * initial geometry.
 * ----------------------------------------------------------- */

static gboolean
gowl_session_default_load_impl(GowlSessionProvider *provider,
                                gpointer             compositor_ptr,
                                GFile               *src,
                                GError             **error)
{
	GowlCompositor         *compositor;
	g_autofree gchar       *path = NULL;
	g_autoptr(YamlParser)   parser = NULL;
	YamlNode               *root;
	YamlMapping            *mapping;
	GowlConfig             *config;

	(void)provider;

	if (!GOWL_IS_COMPOSITOR(compositor_ptr)) {
		g_set_error_literal(error, G_IO_ERROR,
		                    G_IO_ERROR_INVALID_ARGUMENT,
		                    "compositor argument is not a GowlCompositor");
		return FALSE;
	}
	compositor = (GowlCompositor *)compositor_ptr;

	path = g_file_get_path(src);
	if (path == NULL) {
		g_set_error_literal(error, G_IO_ERROR,
		                    G_IO_ERROR_INVALID_ARGUMENT,
		                    "session file has no local path");
		return FALSE;
	}

	parser = yaml_parser_new();
	if (!yaml_parser_load_from_file(parser, path, error))
		return FALSE;

	root = yaml_parser_get_root(parser);
	if (root == NULL
	    || yaml_node_get_node_type(root) != YAML_NODE_MAPPING) {
		g_set_error_literal(error, G_IO_ERROR,
		                    G_IO_ERROR_INVALID_DATA,
		                    "session root is not a mapping");
		return FALSE;
	}
	mapping = yaml_node_get_mapping(root);

	/* Apply monitor state (best-effort: match by index). */
	if (yaml_mapping_has_member(mapping, "monitors")) {
		YamlSequence *seq = yaml_mapping_get_sequence_member(
			mapping, "monitors");
		GList        *mons = gowl_compositor_get_monitors(compositor);
		guint         n_entries = yaml_sequence_get_length(seq);
		guint         i;

		for (i = 0; i < n_entries; i++) {
			YamlMapping *entry;
			gint64       idx;
			GowlMonitor *m;

			entry = yaml_sequence_get_mapping_element(seq, i);
			if (entry == NULL)
				continue;
			if (!yaml_mapping_has_member(entry, "index"))
				continue;

			idx = yaml_mapping_get_int_member(entry, "index");
			m = g_list_nth_data(mons, (guint)idx);
			if (m == NULL)
				continue;

			if (yaml_mapping_has_member(entry, "mfact"))
				gowl_monitor_set_mfact(
					m,
					yaml_mapping_get_double_member(
						entry, "mfact"));
			if (yaml_mapping_has_member(entry, "nmaster"))
				gowl_monitor_set_nmaster(
					m,
					(gint)yaml_mapping_get_int_member(
						entry, "nmaster"));
		}
	}

	/* Apply client entries as transient rules.  When a client maps
	 * later with the recorded app_id, the rule-provider applies
	 * the saved tags/floating/geometry. */
	config = gowl_compositor_get_config(compositor);
	if (config != NULL
	    && yaml_mapping_has_member(mapping, "clients")) {
		YamlSequence *seq;
		guint         n_entries;
		guint         i;

		seq = yaml_mapping_get_sequence_member(mapping, "clients");
		n_entries = yaml_sequence_get_length(seq);

		for (i = 0; i < n_entries; i++) {
			YamlMapping *entry;
			const gchar *app_id, *title;
			guint32      tags;
			gboolean     floating;
			gint         width, height;

			entry = yaml_sequence_get_mapping_element(seq, i);
			if (entry == NULL)
				continue;

			app_id = yaml_mapping_get_string_member(entry, "app_id");
			title  = yaml_mapping_get_string_member(entry, "title");
			tags   = (guint32)yaml_mapping_get_int_member(
				entry, "tags");
			floating = yaml_mapping_has_member(entry, "floating")
				&& yaml_mapping_get_boolean_member(
					entry, "floating");
			width  = 0;
			height = 0;

			if (yaml_mapping_has_member(entry, "geometry")) {
				YamlSequence *geo = yaml_mapping_get_sequence_member(
					entry, "geometry");
				if (geo != NULL
				    && yaml_sequence_get_length(geo) >= 4) {
					width  = (gint)yaml_sequence_get_int_element(
						geo, 2);
					height = (gint)yaml_sequence_get_int_element(
						geo, 3);
				}
			}

			if ((app_id == NULL || *app_id == '\0')
			    && (title == NULL || *title == '\0'))
				continue;

			gowl_config_add_rule_full(
				config,
				app_id,
				title,
				tags,
				floating,
				-1,        /* monitor: any */
				width,
				height,
				TRUE,      /* center */
				FALSE);    /* regex */
		}
	}

	return TRUE;
}

static void
gowl_session_default_iface_init(GowlSessionProviderInterface *iface)
{
	iface->save = gowl_session_default_save_impl;
	iface->load = gowl_session_default_load_impl;
}

static void
gowl_session_default_class_init(GowlSessionDefaultClass *klass)
{
	(void)klass;
}

static void
gowl_session_default_init(GowlSessionDefault *self)
{
	(void)self;
}

GowlSessionDefault *
gowl_session_default_new(void)
{
	return g_object_new(GOWL_TYPE_SESSION_DEFAULT, NULL);
}
