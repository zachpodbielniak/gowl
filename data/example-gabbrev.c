/*
 * gowl - Example C configuration: gabbrev (text expansion)
 * GObject Wayland Compositor
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
 *
 * Three ways a C config extends gabbrev (docs/gabbrev.org, "Extending
 * it"):
 *
 *   1. sources: your espanso configuration, a snippet file, and YAML
 *      written right here -- process-wide, so they survive a config
 *      reload;
 *   2. a C function a YAML variable calls by name:
 *      {type: c, params: {func: NAME}};
 *   3. a new variable TYPE, answered from a thread of its own.
 *
 * Merge add_gabbrev() into ~/.config/gowl/config.c and call it from
 * gowl_config_init(); then `gowl --recompile'.  The module is opt-in:
 *
 *   modules:
 *     gabbrev: { enabled: true }
 */

#include <gowl/gowl.h>

/* 2. `;host' types this machine's name, without a shell */
static gchar *
example_hostname(
	GowlGabbrevRequest *req,
	gpointer            data
){
	(void)req;
	(void)data;
	return g_strdup(g_get_host_name());
}

/* 3. A type `count': how many characters its params.text has.  It
   answers from another thread, which every type may: the request is
   reference counted and its answer is posted back to the compositor. */
static gpointer
example_count_thread(gpointer data)
{
	GowlGabbrevRequest *req = data;
	const gchar *text = gowl_gabbrev_request_get_param(req, "text");
	g_autofree gchar *n = g_strdup_printf("%ld",
		text != NULL ? g_utf8_strlen(text, -1) : 0L);

	gowl_gabbrev_request_return(req, n);
	gowl_gabbrev_request_unref(req);
	return NULL;
}

static void
example_count(
	GowlGabbrevRequest *req,
	gpointer            data
){
	(void)data;
	g_thread_unref(g_thread_new("gabbrev-count", example_count_thread,
	                            gowl_gabbrev_request_ref(req)));
}

/*
 * add_gabbrev:
 *
 * Everything above, registered.  Safe before the module loads.
 *
 * Returns: %TRUE when the inline YAML parsed
 */
static gboolean
add_gabbrev(void)
{
	GError *error = NULL;

	/* 1. sources */
	gowl_gabbrev_add_file("~/.config/espanso");
	gowl_gabbrev_add_file("~/notes/snippets.yml");
	if (!gowl_gabbrev_add_yaml(
		    "matches:\n"
		    "  - trigger: ';host'\n"
		    "    replace: '{{h}}'\n"
		    "    vars: [{name: h, type: c, params: {func: example-hostname}}]\n"
		    "  - trigger: ';len'\n"
		    "    replace: '{{clip}} has {{n}} characters'\n"
		    "    vars:\n"
		    "      - {name: clip, type: clipboard}\n"
		    "      - {name: n, type: count, params: {text: '{{clip}}'}}\n",
		    &error)) {
		g_warning("gabbrev example: %s", error->message);
		g_error_free(error);
		return FALSE;
	}

	/* 2. and 3. */
	gowl_gabbrev_register_func("example-hostname", example_hostname,
	                           NULL, NULL);
	gowl_gabbrev_register_var_type("count", example_count, NULL, NULL);
	return TRUE;
}
