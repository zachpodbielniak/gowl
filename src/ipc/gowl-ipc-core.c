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
 * The compositor's own IPC commands: what a script asks the socket.
 *
 * Every reply is one line.  Queries answer JSON on one line (a client
 * reads a line and parses it); everything else answers `OK ...' or
 * `ERROR ...'.  Anything not known here goes to the modules through
 * gowl_compositor_run_command(), which is how `expo', `screenshot-area'
 * and `scratchpad-toggle' reach their module, so the socket speaks the
 * same names a keybind's `ipc-command' action does.
 *
 * The events a subscriber receives are pushed from the compositor as
 * things happen: see docs/ipc.org for the list.
 */

#include "../core/gowl-core-private.h"
#include "../core/gowl-layout-registry.h"
#include "gowl-ipc.h"
#include "../config/gowl-keybind.h"
#include <json-glib/json-glib.h>
#include <string.h>
#include <stdlib.h>

static gchar *
json_finish(JsonBuilder *b)
{
	JsonGenerator *gen = json_generator_new();
	JsonNode *root = json_builder_get_root(b);
	gchar *out;

	json_generator_set_root(gen, root);
	json_generator_set_pretty(gen, FALSE);
	out = json_generator_to_data(gen, NULL);
	json_node_unref(root);
	g_object_unref(gen);
	g_object_unref(b);
	return out;
}

static void
add_client(JsonBuilder *b, GowlClient *c)
{
	json_builder_begin_object(b);
	json_builder_set_member_name(b, "id");
	json_builder_add_int_value(b, c->id);
	json_builder_set_member_name(b, "app_id");
	json_builder_add_string_value(b, c->app_id != NULL ? c->app_id : "");
	json_builder_set_member_name(b, "title");
	json_builder_add_string_value(b, c->title != NULL ? c->title : "");
	json_builder_set_member_name(b, "pid");
	json_builder_add_int_value(b, gowl_client_get_pid(c));
	json_builder_set_member_name(b, "tags");
	json_builder_add_int_value(b, c->tags);
	json_builder_set_member_name(b, "monitor");
	json_builder_add_string_value(b,
		c->mon != NULL && c->mon->wlr_output != NULL
		? c->mon->wlr_output->name : "");
	json_builder_set_member_name(b, "floating");
	json_builder_add_boolean_value(b, c->isfloating);
	json_builder_set_member_name(b, "fullscreen");
	json_builder_add_boolean_value(b, c->isfullscreen);
	json_builder_set_member_name(b, "urgent");
	json_builder_add_boolean_value(b, c->isurgent);
	json_builder_set_member_name(b, "sticky");
	json_builder_add_boolean_value(b, c->issticky);
	json_builder_set_member_name(b, "xwayland");
	json_builder_add_boolean_value(b, gowl_client_get_xwayland(c));
	json_builder_set_member_name(b, "x");
	json_builder_add_int_value(b, c->geom.x);
	json_builder_set_member_name(b, "y");
	json_builder_add_int_value(b, c->geom.y);
	json_builder_set_member_name(b, "width");
	json_builder_add_int_value(b, c->geom.width);
	json_builder_set_member_name(b, "height");
	json_builder_add_int_value(b, c->geom.height);
	json_builder_end_object(b);
}

static guint32
occupied_tags(GowlCompositor *self, GowlMonitor *m, guint32 *urgent)
{
	guint32 occ = 0;
	GList *l;

	*urgent = 0;
	for (l = self->clients; l != NULL; l = l->next) {
		GowlClient *c = (GowlClient *)l->data;

		if (c->mon != m || c->isembedded || c->isoverlay)
			continue;
		occ |= c->tags;
		if (c->isurgent)
			*urgent |= c->tags;
	}
	return occ;
}

static void
add_monitor(GowlCompositor *self, JsonBuilder *b, GowlMonitor *m)
{
	GowlLayoutEntry *layout = gowl_layout_get(self, m);
	guint32 urgent;
	guint32 occ = occupied_tags(self, m, &urgent);

	json_builder_begin_object(b);
	json_builder_set_member_name(b, "name");
	json_builder_add_string_value(b,
		m->wlr_output != NULL ? m->wlr_output->name : "");
	json_builder_set_member_name(b, "make");
	json_builder_add_string_value(b,
		m->wlr_output != NULL && m->wlr_output->make != NULL
		? m->wlr_output->make : "");
	json_builder_set_member_name(b, "model");
	json_builder_add_string_value(b,
		m->wlr_output != NULL && m->wlr_output->model != NULL
		? m->wlr_output->model : "");
	json_builder_set_member_name(b, "serial");
	json_builder_add_string_value(b,
		m->wlr_output != NULL && m->wlr_output->serial != NULL
		? m->wlr_output->serial : "");
	json_builder_set_member_name(b, "focused");
	json_builder_add_boolean_value(b, m == self->selmon);
	json_builder_set_member_name(b, "enabled");
	json_builder_add_boolean_value(b,
		m->wlr_output != NULL && m->wlr_output->enabled);
	json_builder_set_member_name(b, "powered_off");
	json_builder_add_boolean_value(b, m->powered_off);
	json_builder_set_member_name(b, "x");
	json_builder_add_int_value(b, m->m.x);
	json_builder_set_member_name(b, "y");
	json_builder_add_int_value(b, m->m.y);
	json_builder_set_member_name(b, "width");
	json_builder_add_int_value(b, m->m.width);
	json_builder_set_member_name(b, "height");
	json_builder_add_int_value(b, m->m.height);
	json_builder_set_member_name(b, "tags");
	json_builder_add_int_value(b, m->tagset[m->seltags]);
	json_builder_set_member_name(b, "occupied");
	json_builder_add_int_value(b, occ);
	json_builder_set_member_name(b, "urgent");
	json_builder_add_int_value(b, urgent);
	json_builder_set_member_name(b, "layout");
	json_builder_add_string_value(b,
		layout != NULL && layout->name != NULL ? layout->name : "");
	json_builder_set_member_name(b, "mfact");
	json_builder_add_double_value(b, m->mfact);
	json_builder_set_member_name(b, "nmaster");
	json_builder_add_int_value(b, m->nmaster);
	json_builder_end_object(b);
}

static GowlClient *
client_by_id(GowlCompositor *self, const gchar *arg)
{
	guint id;
	GList *l;

	if (arg == NULL || *arg == '\0')
		return NULL;
	id = (guint)strtoul(arg, NULL, 10);
	for (l = self->clients; l != NULL; l = l->next)
		if (((GowlClient *)l->data)->id == id)
			return (GowlClient *)l->data;
	return NULL;
}

static GowlMonitor *
monitor_by_name(GowlCompositor *self, const gchar *arg)
{
	GList *l;

	if (arg == NULL || *arg == '\0')
		return self->selmon;
	for (l = self->monitors; l != NULL; l = l->next) {
		GowlMonitor *m = (GowlMonitor *)l->data;

		if (m->wlr_output != NULL
		    && g_strcmp0(m->wlr_output->name, arg) == 0)
			return m;
	}
	return NULL;
}

/* Runs a keybind action by nick with an argument: `action tag-view 4'. */
static gchar *
run_action(GowlCompositor *self, const gchar *args)
{
	g_auto(GStrv) parts = NULL;
	GEnumClass *klass;
	GEnumValue *val;
	g_autofree gchar *norm = NULL;
	GowlKeybindEntry kb;
	gboolean ok;

	if (args == NULL || *args == '\0')
		return g_strdup("ERROR action needs a name");
	parts = g_strsplit(args, " ", 2);
	norm = g_strdup(parts[0]);
	g_strdelimit(norm, "_", '-');
	klass = (GEnumClass *)g_type_class_ref(GOWL_TYPE_ACTION);
	val = g_enum_get_value_by_nick(klass, norm);
	g_type_class_unref(klass);
	if (val == NULL)
		return g_strdup_printf("ERROR unknown action '%s'", parts[0]);

	memset(&kb, 0, sizeof kb);
	kb.action = val->value;
	kb.arg = (parts[1] != NULL && *parts[1] != '\0') ? parts[1] : NULL;
	ok = gowl_compositor_run_keybind_entry(self, &kb);
	return g_strdup(ok ? "OK" : "ERROR action did nothing");
}

static const gchar *const help_text =
	"clients | focused | monitors | tags [OUTPUT] | layouts | layout [NAME] | "
	"mode [NAME] | keybinds | keyboard-layout [next|prev|N] | "
	"focus ID | close ID | view TAGMASK [OUTPUT] | "
	"dispatch KEY | action NAME [ARG] | power on|off|toggle | "
	"reload | version | ping | subscribe | help";

/**
 * gowl_compositor_ipc_command:
 * @self: the compositor
 * @line: the command line, without newline
 *
 * The socket's commands.  Core queries and actions here; anything
 * else goes to the modules.
 *
 * Returns: (transfer full) (nullable): the reply line, %NULL for an
 *   unknown command
 */
gchar *
gowl_compositor_ipc_command(
	GowlCompositor *self,
	const gchar    *line
){
	g_autofree gchar *word = NULL;
	const gchar *args = NULL;
	const gchar *space;
	JsonBuilder *b;
	GList *l;

	g_return_val_if_fail(GOWL_IS_COMPOSITOR(self), NULL);
	g_return_val_if_fail(line != NULL, NULL);

	space = strchr(line, ' ');
	if (space != NULL) {
		word = g_strndup(line, (gsize)(space - line));
		args = space + 1;
		while (*args == ' ')
			args++;
	} else {
		word = g_strdup(line);
	}

	if (g_strcmp0(word, "help") == 0)
		return g_strdup(help_text);
	if (g_strcmp0(word, "ping") == 0)
		return g_strdup("pong");
	if (g_strcmp0(word, "version") == 0)
		return g_strdup("gowl " GOWL_VERSION);

	if (g_strcmp0(word, "clients") == 0) {
		b = json_builder_new();
		json_builder_begin_array(b);
		for (l = self->clients; l != NULL; l = l->next) {
			GowlClient *c = (GowlClient *)l->data;

			if (!c->isembedded)
				add_client(b, c);
		}
		json_builder_end_array(b);
		return json_finish(b);
	}
	if (g_strcmp0(word, "focused") == 0) {
		GowlClient *c = gowl_compositor_get_focused_client(self);

		if (c == NULL)
			return g_strdup("null");
		b = json_builder_new();
		add_client(b, c);
		return json_finish(b);
	}
	if (g_strcmp0(word, "monitors") == 0) {
		b = json_builder_new();
		json_builder_begin_array(b);
		for (l = self->monitors; l != NULL; l = l->next)
			add_monitor(self, b, (GowlMonitor *)l->data);
		json_builder_end_array(b);
		return json_finish(b);
	}
	if (g_strcmp0(word, "profile") == 0 || g_strcmp0(word, "profiles") == 0) {
		const gchar *active = gowl_compositor_get_output_profile(self);

		b = json_builder_new();
		json_builder_begin_object(b);
		json_builder_set_member_name(b, "active");
		if (active != NULL)
			json_builder_add_string_value(b, active);
		else
			json_builder_add_null_value(b);
		json_builder_set_member_name(b, "profiles");
		json_builder_begin_array(b);
		for (l = self->config != NULL
		         ? gowl_config_get_output_profiles(self->config) : NULL;
		     l != NULL; l = l->next) {
			const GowlOutputProfile *p = (const GowlOutputProfile *)l->data;
			GHashTableIter iter;
			gpointer k;

			json_builder_begin_object(b);
			json_builder_set_member_name(b, "name");
			json_builder_add_string_value(b, p->name);
			json_builder_set_member_name(b, "outputs");
			json_builder_begin_array(b);
			g_hash_table_iter_init(&iter, p->outputs);
			while (g_hash_table_iter_next(&iter, &k, NULL))
				json_builder_add_string_value(b, (const gchar *)k);
			json_builder_end_array(b);
			json_builder_end_object(b);
		}
		json_builder_end_array(b);
		json_builder_end_object(b);
		return json_finish(b);
	}
	if (g_strcmp0(word, "tags") == 0) {
		GowlMonitor *m = monitor_by_name(self, args);
		guint32 urgent, occ;

		if (m == NULL)
			return g_strdup("ERROR no such output");
		occ = occupied_tags(self, m, &urgent);
		b = json_builder_new();
		json_builder_begin_object(b);
		json_builder_set_member_name(b, "active");
		json_builder_add_int_value(b, m->tagset[m->seltags]);
		json_builder_set_member_name(b, "occupied");
		json_builder_add_int_value(b, occ);
		json_builder_set_member_name(b, "urgent");
		json_builder_add_int_value(b, urgent);
		json_builder_end_object(b);
		return json_finish(b);
	}
	if (g_strcmp0(word, "layouts") == 0) {
		GList *names = gowl_layout_list(self);

		b = json_builder_new();
		json_builder_begin_array(b);
		for (l = names; l != NULL; l = l->next)
			json_builder_add_string_value(b, (const gchar *)l->data);
		json_builder_end_array(b);
		g_list_free(names);
		return json_finish(b);
	}
	if (g_strcmp0(word, "layout") == 0) {
		GowlLayoutEntry *e;

		if (args != NULL && *args != '\0')
			return g_strdup(gowl_layout_set(self, NULL, args)
			                ? "OK" : "ERROR no such layout");
		e = gowl_layout_get(self, NULL);
		return g_strdup(e != NULL && e->name != NULL ? e->name : "");
	}
	if (g_strcmp0(word, "mode") == 0) {
		if (args != NULL && *args != '\0')
			gowl_compositor_set_key_mode(self, args);
		return g_strdup(gowl_compositor_get_key_mode(self));
	}
	if (g_strcmp0(word, "keybinds") == 0) {
		GArray *kbs = self->config != NULL
		              ? gowl_config_get_keybinds(self->config) : NULL;
		GEnumClass *klass = (GEnumClass *)g_type_class_ref(GOWL_TYPE_ACTION);
		guint i;

		b = json_builder_new();
		json_builder_begin_array(b);
		for (i = 0; kbs != NULL && i < kbs->len; i++) {
			GowlKeybindEntry *kb = &g_array_index(kbs, GowlKeybindEntry, i);
			g_autofree gchar *key =
				gowl_keybind_to_string(kb->modifiers, kb->keysym);
			GEnumValue *v = g_enum_get_value(klass, kb->action);

			json_builder_begin_object(b);
			json_builder_set_member_name(b, "key");
			json_builder_add_string_value(b, key);
			json_builder_set_member_name(b, "action");
			json_builder_add_string_value(b, v != NULL ? v->value_nick : "");
			json_builder_set_member_name(b, "arg");
			json_builder_add_string_value(b, kb->arg != NULL ? kb->arg : "");
			json_builder_set_member_name(b, "desc");
			json_builder_add_string_value(b, kb->desc != NULL ? kb->desc : "");
			json_builder_set_member_name(b, "mode");
			json_builder_add_string_value(b, kb->mode != NULL ? kb->mode : "default");
			json_builder_end_object(b);
		}
		json_builder_end_array(b);
		g_type_class_unref(klass);
		return json_finish(b);
	}
	if (g_strcmp0(word, "keyboard-layout") == 0) {
		const gchar *name;
		guint index = 0;

		if (args != NULL && *args != '\0')
			gowl_compositor_switch_keyboard_layout(self, args);
		name = gowl_compositor_get_keyboard_layout(self, &index);
		b = json_builder_new();
		json_builder_begin_object(b);
		json_builder_set_member_name(b, "name");
		json_builder_add_string_value(b, name != NULL ? name : "");
		json_builder_set_member_name(b, "index");
		json_builder_add_int_value(b, index);
		json_builder_end_object(b);
		return json_finish(b);
	}
	if (g_strcmp0(word, "focus") == 0) {
		GowlClient *c = client_by_id(self, args);

		if (c == NULL)
			return g_strdup("ERROR no such client");
		gowl_compositor_activate_client(self, c);
		return g_strdup("OK");
	}
	if (g_strcmp0(word, "close") == 0) {
		GowlClient *c = client_by_id(self, args);

		if (c == NULL)
			return g_strdup("ERROR no such client");
		gowl_client_close(c);
		return g_strdup("OK");
	}
	if (g_strcmp0(word, "view") == 0) {
		g_auto(GStrv) parts = args != NULL ? g_strsplit(args, " ", 2) : NULL;
		GowlMonitor *m;
		guint32 mask;

		if (parts == NULL || parts[0] == NULL)
			return g_strdup("ERROR view needs a tag mask");
		mask = (guint32)strtoul(parts[0], NULL, 10);
		m = monitor_by_name(self, parts[1]);
		if (m == NULL)
			return g_strdup("ERROR no such output");
		if (mask == 0)
			return g_strdup("ERROR tag mask must not be 0");
		gowl_monitor_set_tags(m, mask);
		self->selmon = m;
		gowl_compositor_focus_client(self, focustop_public(self, m), TRUE);
		gowl_compositor_arrange(self, m);
		return g_strdup("OK");
	}
	if (g_strcmp0(word, "dispatch") == 0) {
		guint mods, sym;

		if (args == NULL || !gowl_keybind_parse(args, &mods, &sym))
			return g_strdup("ERROR dispatch needs a key such as Super+Return");
		return g_strdup(gowl_compositor_dispatch_keybind(self, mods, sym)
		                ? "OK" : "ERROR nothing bound");
	}
	if (g_strcmp0(word, "action") == 0)
		return run_action(self, args);
	if (g_strcmp0(word, "power") == 0) {
		gboolean on;

		if (args != NULL && g_ascii_strcasecmp(args, "on") == 0)
			on = TRUE;
		else if (args != NULL && g_ascii_strcasecmp(args, "off") == 0)
			on = FALSE;
		else
			on = gowl_compositor_any_output_powered_off(self);
		gowl_compositor_set_outputs_powered(self, on);
		return g_strdup(on ? "OK on" : "OK off");
	}
	if (g_strcmp0(word, "reload") == 0
	    || g_strcmp0(word, "reload_config") == 0
	    || g_strcmp0(word, "reload-config") == 0) {
		GowlKeybindEntry kb;

		memset(&kb, 0, sizeof kb);
		kb.action = GOWL_ACTION_RELOAD_CONFIG;
		gowl_compositor_run_keybind_entry(self, &kb);
		return g_strdup(self->config != NULL
		                && gowl_config_get_problem_count(self->config) > 0
		                ? "OK with problems" : "OK");
	}

	/* A module's command, or nothing. */
	return gowl_compositor_run_command(self, line);
}
