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
 * gowl-hotkey-model.c - Reading a `hotkey:' section.
 *
 * The section (docs/hotkey.org has every key):
 *
 *   hotkey:
 *     include:    [other.yaml, ...]       more sections, relative to this
 *     scripts:    [hotkeys.c, ...]        crispy hotkey scripts
 *     contexts:   {NAME: FILTER | {when:, over:, exists:, absent:}}
 *     groups:     {NAME: FILTER | {...}}  window groups (group-activate)
 *     hotkeys:    [{key:, do:, context:, when:, mode:, double:, ...}]
 *     remaps:     {FROM: TO} | [{from:, to:, context:, ...}]
 *     modes:      {NAME: {timeout:, oneshot:, swallow:, hotkeys:, ...}}
 *     hotstrings: [":opts:abbr::text" | {hotstring:, do:, ...}]
 *     timers:     [{every: | once:, do:, ...}]
 *     on:         {EVENT: [{filter:, do:}] | [ACTIONS]}
 *     functions:  {NAME: [ACTIONS]}
 *
 * An action is a mapping with exactly one kind key (`{send: "^c"}',
 * `{action: spawn, arg: gst}', `{shell: "..."}') plus its options; a
 * bare string is a gowl command line.  Every unknown key, bad key name
 * or unparseable filter is one line in `problems', and only the entry
 * it is in is skipped.
 */

#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "gowl-hotkey"

#include "gowl-hotkey-model.h"

#include <gio/gio.h>
#include <linux/input-event-codes.h>
#include <string.h>

/* ── Small helpers ─────────────────────────────────────────────────── */

static void G_GNUC_PRINTF(2, 3)
problem(
	GPtrArray   *problems,
	const gchar *format,
	...
){
	va_list ap;
	gchar *msg;

	va_start(ap, format);
	msg = g_strdup_vprintf(format, ap);
	va_end(ap);
	if (problems != NULL)
		g_ptr_array_add(problems, msg);
	else
		g_free(msg);
}

static const gchar *
node_scalar(YamlNode *node)
{
	if (node == NULL || yaml_node_get_node_type(node) != YAML_NODE_SCALAR)
		return NULL;
	return yaml_node_get_scalar(node);
}

static gboolean
is_mapping(YamlNode *node)
{
	return node != NULL && yaml_node_get_node_type(node) == YAML_NODE_MAPPING;
}

static gboolean
is_sequence(YamlNode *node)
{
	return node != NULL && yaml_node_get_node_type(node) == YAML_NODE_SEQUENCE;
}

static YamlNode *
member(
	YamlMapping *m,
	const gchar *name
){
	gchar *alt;
	YamlNode *n;

	if (yaml_mapping_has_member(m, name))
		return yaml_mapping_get_member(m, name);
	/* keys may be spelt with `_' or `-' */
	alt = g_strdup(name);
	g_strdelimit(alt, "-", '_');
	n = yaml_mapping_has_member(m, alt) ? yaml_mapping_get_member(m, alt)
	                                     : NULL;
	g_free(alt);
	return n;
}

/* yes/no: TRUE when @text is one; the value in @out */
static gboolean
parse_bool(
	const gchar *text,
	gboolean    *out
){
	if (text == NULL)
		return FALSE;
	if (g_ascii_strcasecmp(text, "true") == 0
	    || g_ascii_strcasecmp(text, "yes") == 0
	    || g_ascii_strcasecmp(text, "on") == 0
	    || g_strcmp0(text, "1") == 0) {
		*out = TRUE;
		return TRUE;
	}
	if (g_ascii_strcasecmp(text, "false") == 0
	    || g_ascii_strcasecmp(text, "no") == 0
	    || g_ascii_strcasecmp(text, "off") == 0
	    || g_strcmp0(text, "0") == 0) {
		*out = FALSE;
		return TRUE;
	}
	return FALSE;
}

/*
 * A duration: a number of milliseconds, or a number with ms, s, m or
 * h after it (`250', `250ms', `1.5s', `10m', `1h').
 */
static gboolean
parse_duration(
	const gchar *text,
	guint       *out
){
	gchar *end = NULL;
	gdouble v;
	gdouble scale = 1.0;

	if (text == NULL || *text == '\0')
		return FALSE;
	v = g_ascii_strtod(text, &end);
	if (end == text || v < 0)
		return FALSE;
	while (*end == ' ')
		end++;
	if (*end == '\0' || g_ascii_strcasecmp(end, "ms") == 0)
		scale = 1.0;
	else if (g_ascii_strcasecmp(end, "s") == 0)
		scale = 1000.0;
	else if (g_ascii_strcasecmp(end, "m") == 0
	         || g_ascii_strcasecmp(end, "min") == 0)
		scale = 60000.0;
	else if (g_ascii_strcasecmp(end, "h") == 0)
		scale = 3600000.0;
	else
		return FALSE;
	v *= scale;
	if (v > (gdouble)G_MAXUINT)
		return FALSE;
	*out = (guint)(v + 0.5);
	return TRUE;
}

static gboolean
parse_int(
	const gchar *text,
	gint64       min,
	gint64       max,
	gint64      *out
){
	return text != NULL
	       && g_ascii_string_to_signed(text, 10, min, max, out, NULL);
}

/* A list of strings: a sequence of scalars, or one scalar. */
static gchar **
string_list(YamlNode *node)
{
	GPtrArray *out;
	guint i;

	if (node == NULL)
		return NULL;
	out = g_ptr_array_new();
	if (is_sequence(node)) {
		YamlSequence *seq = yaml_node_get_sequence(node);

		for (i = 0; i < yaml_sequence_get_length(seq); i++) {
			const gchar *s = node_scalar(yaml_sequence_get_element(seq, i));

			if (s != NULL)
				g_ptr_array_add(out, g_strdup(s));
		}
	} else if (node_scalar(node) != NULL) {
		g_ptr_array_add(out, g_strdup(node_scalar(node)));
	}
	g_ptr_array_add(out, NULL);
	return (gchar **)g_ptr_array_free(out, FALSE);
}

/* Reports every key of @m not in @known (NULL-terminated). */
static void
check_keys(
	YamlMapping         *m,
	const gchar * const *known,
	const gchar         *where,
	GPtrArray           *problems
){
	guint i;
	guint j;

	for (i = 0; i < yaml_mapping_get_size(m); i++) {
		const gchar *k = yaml_mapping_get_key(m, i);
		gboolean ok = FALSE;
		g_autofree gchar *norm = g_strdup(k);

		g_strdelimit(norm, "_", '-');
		for (j = 0; known[j] != NULL; j++)
			if (g_strcmp0(known[j], norm) == 0) {
				ok = TRUE;
				break;
			}
		if (!ok && !g_str_has_prefix(k, "x-"))
			problem(problems, "%s: unknown key `%s'", where, k);
	}
}

/* ── Fields ────────────────────────────────────────────────────────── */

static const gchar * const window_fields[] = {
	"app-id", "class", "title", "exec", "pid", "id", "xwayland",
	"floating", "fullscreen", "monitor", "layout", "tags", "tag", "time",
	"hour", "weekday", "mode", NULL
};

/**
 * hk_window_fields: (skip)
 *
 * Returns: (transfer none): the field names a hotkey filter may use
 */
const gchar * const *
hk_window_fields(void)
{
	return window_fields;
}

/* ── Actions ───────────────────────────────────────────────────────── */

/**
 * hk_action_free: (skip)
 * @action: (transfer full): an action
 */
void
hk_action_free(HkAction *action)
{
	if (action == NULL)
		return;
	g_free(action->name);
	g_free(action->text);
	g_strfreev(action->argv);
	if (action->params != NULL)
		json_object_unref(action->params);
	g_free(action->target);
	g_free(action->cflags);
	g_free(action);
}

static const struct {
	const gchar *key;
	HkActKind    kind;
} action_keys[] = {
	{ "action",     HK_ACT_ACTION },
	{ "command",    HK_ACT_COMMAND },
	{ "rpc",        HK_ACT_RPC },
	{ "window",     HK_ACT_WINDOW },
	{ "macro",      HK_ACT_MACRO },
	{ "shell",      HK_ACT_SHELL },
	{ "run",        HK_ACT_RUN },
	{ "c",          HK_ACT_C },
	{ "call",       HK_ACT_CALL },
	{ "send",       HK_ACT_SEND },
	{ "send-text",  HK_ACT_SEND_TEXT },
	{ "mode",       HK_ACT_MODE },
	{ "suspend",    HK_ACT_SUSPEND },
	{ "reload",     HK_ACT_RELOAD },
	{ "mouse",      HK_ACT_MOUSE },
	{ "clipboard",  HK_ACT_CLIPBOARD },
	{ "notify",     HK_ACT_NOTIFY },
	{ "toast",      HK_ACT_NOTIFY },
	{ "sleep",      HK_ACT_SLEEP },
	{ "custom",     HK_ACT_CUSTOM },
	{ "elisp",      HK_ACT_CUSTOM },
	{ "input",      HK_ACT_INPUT },
	{ "context",    HK_ACT_CONTEXT },
	{ "capslock",   HK_ACT_LOCK },
	{ "numlock",    HK_ACT_LOCK },
	{ "hotkey",     HK_ACT_HOTKEY },
	{ NULL,         HK_ACT_ACTION }
};

/**
 * hk_action_kind_name: (skip)
 * @kind: an action kind
 *
 * Returns: (transfer none): its YAML key
 */
const gchar *
hk_action_kind_name(HkActKind kind)
{
	guint i;

	for (i = 0; action_keys[i].key != NULL; i++)
		if (action_keys[i].kind == kind)
			return action_keys[i].key;
	return "?";
}

/* The options an action mapping may carry beside its kind key. */
static const gchar * const action_options[] = {
	"arg", "args", "params", "target", "wait", "timeout", "delay",
	"key-delay", "send-mode", "level", "cflags", "file", "value",
	"button", "count", "x", "y", "to-x", "to-y", "relative", "coord",
	"notches", "horizontal", "id", "length", "end-keys", "match",
	"visible", "case-sensitive", "name", NULL
};

/* `toggle', `on', `off', `always-on', `always-off', `normal'. */
static gboolean
parse_toggle(
	const gchar *text,
	HkToggle    *out
){
	gboolean b;

	if (text == NULL)
		return FALSE;
	if (g_ascii_strcasecmp(text, "toggle") == 0) {
		*out = HK_TOGGLE_TOGGLE;
		return TRUE;
	}
	if (g_ascii_strcasecmp(text, "always-on") == 0
	    || g_ascii_strcasecmp(text, "alwayson") == 0) {
		*out = HK_TOGGLE_ALWAYS_ON;
		return TRUE;
	}
	if (g_ascii_strcasecmp(text, "always-off") == 0
	    || g_ascii_strcasecmp(text, "alwaysoff") == 0) {
		*out = HK_TOGGLE_ALWAYS_OFF;
		return TRUE;
	}
	if (g_ascii_strcasecmp(text, "normal") == 0) {
		*out = HK_TOGGLE_NORMAL;
		return TRUE;
	}
	if (g_ascii_strcasecmp(text, "enable") == 0) {
		*out = HK_TOGGLE_ON;
		return TRUE;
	}
	if (g_ascii_strcasecmp(text, "disable") == 0) {
		*out = HK_TOGGLE_OFF;
		return TRUE;
	}
	if (parse_bool(text, &b)) {
		*out = b ? HK_TOGGLE_ON : HK_TOGGLE_OFF;
		return TRUE;
	}
	return FALSE;
}

/* A YAML mapping (or nothing) as a JSON object, for rpc params. */
static JsonObject *
params_of(
	YamlNode    *node,
	const gchar *where,
	GPtrArray   *problems
){
	g_autoptr(JsonNode) json = NULL;

	if (node == NULL)
		return json_object_new();
	if (!is_mapping(node)) {
		problem(problems, "%s: params is a mapping", where);
		return json_object_new();
	}
	json = yaml_node_to_json_node(node);
	if (json == NULL || !JSON_NODE_HOLDS_OBJECT(json))
		return json_object_new();
	return json_object_ref(json_node_get_object(json));
}

/* Numbers in `params' come from YAML as strings when quoted; the wire
 * protocol checks types, so a window verb's numbers are put in as
 * numbers. */
static void
set_number(
	JsonObject  *o,
	const gchar *key,
	const gchar *text
){
	gdouble v;
	gchar *end = NULL;

	if (text == NULL)
		return;
	v = g_ascii_strtod(text, &end);
	if (end == text)
		return;
	if (v == (gdouble)(gint64)v)
		json_object_set_int_member(o, key, (gint64)v);
	else
		json_object_set_double_member(o, key, v);
}

/*
 * `{window: VERB, arg: X}' into the wire-protocol method it stands for.
 * The window itself (`id') is filled in when the action runs: the
 * focused one, or the first window matching `target:'.
 */
static gboolean
window_verb(
	HkAction    *a,
	const gchar *verb,
	const gchar *arg,
	const gchar *where,
	GPtrArray   *problems
){
	static const struct {
		const gchar *verb;
		const gchar *method;
		const gchar *value;   /* a fixed `value', or NULL */
	} verbs[] = {
		{ "float",          "window.set_floating",   NULL },
		{ "fullscreen",     "window.set_fullscreen", NULL },
		{ "sticky",         "window.set_sticky",     NULL },
		{ "below",          "window.set_below",      NULL },
		{ "urgent",         "window.set_urgent",     NULL },
		{ "minimize",       "window.set_minimized",  "true" },
		{ "restore",        "window.set_minimized",  "false" },
		{ "close",          "window.close",          NULL },
		{ "kill",           "window.close",          NULL },
		{ "activate",       "window.focus",          NULL },
		{ "focus",          "window.focus",          NULL },
		{ "center",         "window.center",         NULL },
		{ "promote",        "window.promote",        NULL },
		{ "move",           "window.set_geometry",   NULL },
		{ "resize",         "window.set_geometry",   NULL },
		{ "geometry",       "window.set_geometry",   NULL },
		{ "move-by",        "window.move_by",        NULL },
		{ "opacity",        "window.set_opacity",    NULL },
		{ "tag",            "window.set_tags",       NULL },
		{ "send-to-monitor", "window.send_to_monitor", NULL },
		{ "group-activate", NULL,                    NULL },
		{ NULL, NULL, NULL }
	};
	g_auto(GStrv) parts = NULL;
	guint i;

	for (i = 0; verbs[i].verb != NULL; i++)
		if (g_ascii_strcasecmp(verbs[i].verb, verb) == 0)
			break;
	if (verbs[i].verb == NULL) {
		problem(problems, "%s: unknown window verb `%s' (float, fullscreen, "
		        "sticky, below, minimize, restore, close, activate, center, "
		        "promote, move, resize, geometry, move-by, opacity, tag, "
		        "send-to-monitor, group-activate)", where, verb);
		return FALSE;
	}
	g_free(a->name);
	a->name = g_strdup(verbs[i].verb);
	if (verbs[i].method == NULL) {
		/* group-activate: the group's name is the argument */
		if (arg == NULL || *arg == '\0') {
			problem(problems, "%s: group-activate needs a group (arg:)",
			        where);
			return FALSE;
		}
		g_free(a->text);
		a->text = g_strdup(arg);
		return TRUE;
	}
	json_object_set_string_member(a->params, "x-method", verbs[i].method);
	if (arg != NULL) {
		g_autofree gchar *tmp = g_strstrip(g_strdup(arg));

		parts = g_strsplit_set(tmp, " x,", -1);
	}
	if (verbs[i].value != NULL)
		json_object_set_boolean_member(a->params, "value",
		                               g_strcmp0(verbs[i].value, "true") == 0);
	else if (g_str_has_prefix(verbs[i].method, "window.set_")
	         && g_strcmp0(verbs[i].method, "window.set_geometry") != 0
	         && g_strcmp0(verbs[i].method, "window.set_opacity") != 0
	         && g_strcmp0(verbs[i].method, "window.set_tags") != 0
	         && !json_object_has_member(a->params, "value")) {
		HkToggle t = HK_TOGGLE_TOGGLE;

		if (arg != NULL && !parse_toggle(arg, &t)) {
			problem(problems, "%s: %s takes on, off or toggle", where, verb);
			return FALSE;
		}
		if (t == HK_TOGGLE_TOGGLE)
			json_object_set_string_member(a->params, "value", "toggle");
		else
			json_object_set_boolean_member(a->params, "value",
			                               t == HK_TOGGLE_ON);
	}
	if (parts == NULL)
		return TRUE;
	if (g_ascii_strcasecmp(verb, "move") == 0 && g_strv_length(parts) >= 2) {
		set_number(a->params, "x", parts[0]);
		set_number(a->params, "y", parts[1]);
	} else if (g_ascii_strcasecmp(verb, "resize") == 0
	           && g_strv_length(parts) >= 2) {
		set_number(a->params, "width", parts[0]);
		set_number(a->params, "height", parts[1]);
	} else if (g_ascii_strcasecmp(verb, "geometry") == 0
	           && g_strv_length(parts) >= 4) {
		set_number(a->params, "x", parts[0]);
		set_number(a->params, "y", parts[1]);
		set_number(a->params, "width", parts[2]);
		set_number(a->params, "height", parts[3]);
	} else if (g_ascii_strcasecmp(verb, "move-by") == 0
	           && g_strv_length(parts) >= 2) {
		set_number(a->params, "dx", parts[0]);
		set_number(a->params, "dy", parts[1]);
	} else if (g_ascii_strcasecmp(verb, "center") == 0
	           && g_strv_length(parts) >= 2) {
		set_number(a->params, "width", parts[0]);
		set_number(a->params, "height", parts[1]);
	} else if (g_ascii_strcasecmp(verb, "opacity") == 0) {
		set_number(a->params, "opacity", parts[0]);
	} else if (g_ascii_strcasecmp(verb, "tag") == 0) {
		set_number(a->params, "tag", parts[0]);
	} else if (g_ascii_strcasecmp(verb, "send-to-monitor") == 0) {
		json_object_set_string_member(a->params, "monitor", arg);
	}
	return TRUE;
}

/* `left', `right', `middle', `back' (side), `forward' (extra), or an
 * AutoHotkey button name. */
static guint32
mouse_button(const gchar *name)
{
	GowlHotkeyInput in;

	if (name == NULL || g_ascii_strcasecmp(name, "left") == 0)
		return BTN_LEFT;
	if (g_ascii_strcasecmp(name, "right") == 0)
		return BTN_RIGHT;
	if (g_ascii_strcasecmp(name, "middle") == 0)
		return BTN_MIDDLE;
	if (g_ascii_strcasecmp(name, "back") == 0
	    || g_ascii_strcasecmp(name, "side") == 0)
		return BTN_SIDE;
	if (g_ascii_strcasecmp(name, "forward") == 0
	    || g_ascii_strcasecmp(name, "extra") == 0)
		return BTN_EXTRA;
	if (gowl_hotkey_input_parse(name, &in, NULL)
	    && in.kind == GOWL_HOTKEY_INPUT_BUTTON)
		return in.button;
	return 0;
}

static gboolean
parse_mouse(
	HkAction    *a,
	YamlMapping *m,
	const gchar *verb,
	const gchar *where,
	GPtrArray   *problems
){
	const gchar *s;
	gint64 n;

	if (g_ascii_strcasecmp(verb, "click") != 0
	    && g_ascii_strcasecmp(verb, "down") != 0
	    && g_ascii_strcasecmp(verb, "up") != 0
	    && g_ascii_strcasecmp(verb, "move") != 0
	    && g_ascii_strcasecmp(verb, "drag") != 0
	    && g_ascii_strcasecmp(verb, "wheel") != 0) {
		problem(problems, "%s: unknown mouse verb `%s' (click, down, up, "
		        "move, drag, wheel)", where, verb);
		return FALSE;
	}
	a->name = g_ascii_strdown(verb, -1);
	a->count = 1;
	a->button = mouse_button(node_scalar(member(m, "button")));
	if (a->button == 0) {
		problem(problems, "%s: unknown mouse button `%s'", where,
		        node_scalar(member(m, "button")));
		return FALSE;
	}
	if ((s = node_scalar(member(m, "count"))) != NULL) {
		if (!parse_int(s, 1, 20, &n)) {
			problem(problems, "%s: count is 1..20", where);
			return FALSE;
		}
		a->count = (gint)n;
	}
	if (member(m, "x") != NULL || member(m, "y") != NULL) {
		a->has_xy = TRUE;
		a->x = g_ascii_strtod(node_scalar(member(m, "x")) != NULL
		                      ? node_scalar(member(m, "x")) : "0", NULL);
		a->y = g_ascii_strtod(node_scalar(member(m, "y")) != NULL
		                      ? node_scalar(member(m, "y")) : "0", NULL);
	}
	a->x2 = g_ascii_strtod(node_scalar(member(m, "to-x")) != NULL
	                       ? node_scalar(member(m, "to-x")) : "0", NULL);
	a->y2 = g_ascii_strtod(node_scalar(member(m, "to-y")) != NULL
	                       ? node_scalar(member(m, "to-y")) : "0", NULL);
	if (g_ascii_strcasecmp(verb, "drag") == 0
	    && (member(m, "to-x") == NULL || member(m, "to-y") == NULL)) {
		problem(problems, "%s: a drag needs to-x and to-y", where);
		return FALSE;
	}
	parse_bool(node_scalar(member(m, "relative")), &a->relative);
	if ((s = node_scalar(member(m, "coord"))) != NULL) {
		if (g_ascii_strcasecmp(s, "window") == 0)
			a->window_rel = TRUE;
		else if (g_ascii_strcasecmp(s, "screen") != 0) {
			problem(problems, "%s: coord is screen or window", where);
			return FALSE;
		}
	}
	if (g_ascii_strcasecmp(verb, "wheel") == 0) {
		a->wheel = 1;
		if ((s = node_scalar(member(m, "notches"))) != NULL) {
			if (!parse_int(s, -50, 50, &n) || n == 0) {
				problem(problems, "%s: notches is -50..50, not 0 "
				        "(negative scrolls up or left)", where);
				return FALSE;
			}
			a->wheel = (gint)n;
		}
		parse_bool(node_scalar(member(m, "horizontal")), &a->horizontal);
	}
	return TRUE;
}

/* One action from a mapping. */
static HkAction *
parse_action_mapping(
	YamlMapping *m,
	const gchar *where,
	GPtrArray   *problems,
	gboolean     shorthand
){
	HkAction *a;
	const gchar *kind_key = NULL;
	YamlNode *kind_node = NULL;
	const gchar *s;
	guint i;
	guint j;
	guint n_kinds = 0;
	gint64 n;

	/* Exactly one kind key */
	for (i = 0; i < yaml_mapping_get_size(m); i++) {
		g_autofree gchar *norm = g_strdup(yaml_mapping_get_key(m, i));

		g_strdelimit(norm, "_", '-');
		for (j = 0; action_keys[j].key != NULL; j++)
			if (g_strcmp0(action_keys[j].key, norm) == 0) {
				/* On a hotkey entry, mode: and context: are the
				 * hotkey's own, not actions */
				if (shorthand && (action_keys[j].kind == HK_ACT_MODE
				                  || action_keys[j].kind == HK_ACT_CONTEXT))
					break;
				if (n_kinds == 0) {
					kind_key = action_keys[j].key;
					kind_node = yaml_mapping_get_value(m, i);
				}
				n_kinds++;
				break;
			}
	}
	if (n_kinds == 0) {
		problem(problems, "%s: an action needs one of action, command, rpc, "
		        "window, macro, shell, run, c, call, send, send-text, mode, "
		        "suspend, reload, mouse, clipboard, notify, sleep, custom, "
		        "input, context, capslock, numlock, hotkey", where);
		return NULL;
	}
	if (n_kinds > 1) {
		problem(problems, "%s: an action has exactly one kind key; this one "
		        "has %u (make it a list of actions)", where, n_kinds);
		return NULL;
	}
	if (!shorthand) {
		g_autoptr(GPtrArray) known = g_ptr_array_new();

		for (j = 0; action_options[j] != NULL; j++)
			g_ptr_array_add(known, (gpointer)action_options[j]);
		g_ptr_array_add(known, (gpointer)kind_key);
		g_ptr_array_add(known, NULL);
		check_keys(m, (const gchar * const *)known->pdata, where, problems);
	}

	a = g_new0(HkAction, 1);
	for (j = 0; action_keys[j].key != NULL; j++)
		if (g_strcmp0(action_keys[j].key, kind_key) == 0) {
			a->kind = action_keys[j].kind;
			break;
		}
	a->send_mode = -1;
	a->level = -1;
	a->timeout_ms = HK_DEFAULT_TIMEOUT_MS;
	a->params = params_of(member(m, "params"), where, problems);
	a->target = g_strdup(node_scalar(member(m, "target")));
	parse_bool(node_scalar(member(m, "wait")), &a->wait);
	if ((s = node_scalar(member(m, "timeout"))) != NULL
	    && !parse_duration(s, &a->timeout_ms))
		problem(problems, "%s: timeout `%s' is not a duration", where, s);
	if ((s = node_scalar(member(m, "delay"))) != NULL
	    || (s = node_scalar(member(m, "key-delay"))) != NULL) {
		if (!parse_duration(s, &a->ms))
			problem(problems, "%s: delay `%s' is not a duration", where, s);
	}
	if ((s = node_scalar(member(m, "send-mode"))) != NULL) {
		if (g_ascii_strcasecmp(s, "input") == 0)
			a->send_mode = 0;
		else if (g_ascii_strcasecmp(s, "event") == 0)
			a->send_mode = 1;
		else
			problem(problems, "%s: send-mode is input or event", where);
	}
	if ((s = node_scalar(member(m, "level"))) != NULL) {
		if (parse_int(s, 0, HK_MAX_LEVEL, &n))
			a->level = (gint)n;
		else
			problem(problems, "%s: level is 0..%d", where, HK_MAX_LEVEL);
	}
	a->cflags = g_strdup(node_scalar(member(m, "cflags")));
	a->argv = string_list(member(m, "args"));

	s = node_scalar(kind_node);
	switch (a->kind) {
	case HK_ACT_ACTION:
		if (s == NULL) {
			problem(problems, "%s: action names a keybind action, such as "
			        "spawn or kill-client", where);
			goto bad;
		}
		a->name = g_strdup(s);
		g_strdelimit(a->name, "_", '-');
		a->text = g_strdup(node_scalar(member(m, "arg")));
		break;
	case HK_ACT_COMMAND:
	case HK_ACT_SHELL:
	case HK_ACT_SEND:
	case HK_ACT_SEND_TEXT:
	case HK_ACT_CLIPBOARD:
	case HK_ACT_NOTIFY:
	case HK_ACT_CUSTOM:
		if (s == NULL) {
			problem(problems, "%s: %s takes a string", where, kind_key);
			goto bad;
		}
		a->text = g_strdup(s);
		break;
	case HK_ACT_RPC:
		if (s == NULL || strchr(s, '.') == NULL) {
			problem(problems, "%s: rpc names a method, such as "
			        "window.center (gowl-msg -c rpc.discover lists them)",
			        where);
			goto bad;
		}
		a->name = g_strdup(s);
		break;
	case HK_ACT_WINDOW:
		if (s == NULL) {
			problem(problems, "%s: window takes a verb", where);
			goto bad;
		}
		if (!window_verb(a, s, node_scalar(member(m, "arg")), where, problems))
			goto bad;
		break;
	case HK_ACT_MACRO:
	case HK_ACT_CALL:
		if (s == NULL || *s == '\0') {
			problem(problems, "%s: %s names one", where, kind_key);
			goto bad;
		}
		a->name = g_strdup(s);
		break;
	case HK_ACT_RUN:
		if (is_sequence(kind_node)) {
			g_strfreev(a->argv);
			a->argv = string_list(kind_node);
		} else if (s != NULL) {
			GError *e = NULL;

			g_strfreev(a->argv);
			a->argv = NULL;
			if (!g_shell_parse_argv(s, NULL, &a->argv, &e)) {
				problem(problems, "%s: run: %s", where, e->message);
				g_error_free(e);
				goto bad;
			}
		}
		if (a->argv == NULL || a->argv[0] == NULL) {
			problem(problems, "%s: run takes a program and its arguments",
			        where);
			goto bad;
		}
		break;
	case HK_ACT_C:
		if (is_mapping(kind_node)) {
			YamlMapping *cm = yaml_node_get_mapping(kind_node);

			if ((s = node_scalar(member(cm, "file"))) != NULL) {
				a->is_file = TRUE;
				a->text = g_strdup(s);
			} else if ((s = node_scalar(member(cm, "code"))) != NULL) {
				a->text = g_strdup(s);
			}
			if (node_scalar(member(cm, "cflags")) != NULL) {
				g_free(a->cflags);
				a->cflags = g_strdup(node_scalar(member(cm, "cflags")));
			}
		} else if (s != NULL) {
			a->text = g_strdup(s);
		} else if ((s = node_scalar(member(m, "file"))) != NULL) {
			a->is_file = TRUE;
			a->text = g_strdup(s);
		}
		if (a->text == NULL) {
			problem(problems, "%s: c takes the code, or {file: PATH}", where);
			goto bad;
		}
		break;
	case HK_ACT_MODE:
		a->name = g_strdup(s != NULL && *s != '\0' ? s : "default");
		break;
	case HK_ACT_SUSPEND:
	case HK_ACT_LOCK:
		a->toggle = HK_TOGGLE_TOGGLE;
		if (s != NULL && !parse_toggle(s, &a->toggle)) {
			problem(problems, "%s: %s takes on, off or toggle%s", where,
			        kind_key, a->kind == HK_ACT_LOCK
			        ? ", always-on, always-off or normal" : "");
			goto bad;
		}
		if (a->kind == HK_ACT_LOCK)
			a->name = g_strdup(kind_key);
		break;
	case HK_ACT_RELOAD:
		break;
	case HK_ACT_MOUSE:
		if (s == NULL || !parse_mouse(a, m, s, where, problems))
			goto bad;
		break;
	case HK_ACT_SLEEP:
		if (s == NULL || !parse_duration(s, &a->ms)) {
			problem(problems, "%s: sleep takes a duration (250, 1.5s)", where);
			goto bad;
		}
		break;
	case HK_ACT_INPUT:
		/* {input: NAME, length:, timeout:, end-keys:, match:, visible:}
		 * -- the name is what the end event and `{{input}}' carry */
		a->name = g_strdup(s != NULL ? s : "input");
		{
			GPtrArray *opts = g_ptr_array_new();
			const gchar *v;

			if ((v = node_scalar(member(m, "length"))) != NULL)
				g_ptr_array_add(opts, g_strdup_printf("L%s", v));
			if ((v = node_scalar(member(m, "timeout"))) != NULL) {
				guint ms;

				if (parse_duration(v, &ms))
					g_ptr_array_add(opts, g_strdup_printf("T%u", ms));
			}
			if ((v = node_scalar(member(m, "visible"))) != NULL) {
				gboolean b = FALSE;

				if (parse_bool(v, &b) && b)
					g_ptr_array_add(opts, g_strdup("V"));
			}
			if ((v = node_scalar(member(m, "case-sensitive"))) != NULL) {
				gboolean b = FALSE;

				if (parse_bool(v, &b) && b)
					g_ptr_array_add(opts, g_strdup("C"));
			}
			g_ptr_array_add(opts, NULL);
			a->text = g_strjoinv(" ", (gchar **)opts->pdata);
			g_ptr_array_free(opts, TRUE);
		}
		{
			g_auto(GStrv) ends = string_list(member(m, "end-keys"));
			g_auto(GStrv) match = string_list(member(m, "match"));

			/* argv: end keys, then `--', then the match list */
			g_strfreev(a->argv);
			{
				GPtrArray *v = g_ptr_array_new();

				for (i = 0; ends != NULL && ends[i] != NULL; i++)
					g_ptr_array_add(v, g_strdup(ends[i]));
				g_ptr_array_add(v, g_strdup("--"));
				for (i = 0; match != NULL && match[i] != NULL; i++)
					g_ptr_array_add(v, g_strdup(match[i]));
				g_ptr_array_add(v, NULL);
				a->argv = (gchar **)g_ptr_array_free(v, FALSE);
			}
		}
		break;
	case HK_ACT_CONTEXT:
		if (s == NULL || *s == '\0') {
			problem(problems, "%s: context names one", where);
			goto bad;
		}
		a->name = g_strdup(s);
		a->toggle = HK_TOGGLE_ON;
		if ((s = node_scalar(member(m, "value"))) != NULL
		    && !parse_toggle(s, &a->toggle)) {
			problem(problems, "%s: value is true, false or toggle", where);
			goto bad;
		}
		break;
	case HK_ACT_HOTKEY:
		a->toggle = HK_TOGGLE_TOGGLE;
		if (s != NULL && !parse_toggle(s, &a->toggle)) {
			problem(problems, "%s: hotkey takes enable, disable or toggle",
			        where);
			goto bad;
		}
		a->name = g_strdup(node_scalar(member(m, "id")));
		if (a->name == NULL) {
			problem(problems, "%s: hotkey needs the id of the hotkey",
			        where);
			goto bad;
		}
		break;
	default:
		break;
	}
	return a;

bad:
	hk_action_free(a);
	return NULL;
}

/**
 * hk_parse_actions: (skip)
 * @node: an action, or a list of them
 * @where: what to call it in a problem
 * @problems: (nullable): where problems go
 *
 * A bare string is a gowl command line.
 *
 * Returns: (transfer full) (element-type HkAction): the actions; an
 *   action with a problem is left out
 */
GPtrArray *
hk_parse_actions(
	YamlNode    *node,
	const gchar *where,
	GPtrArray   *problems
){
	GPtrArray *out;
	guint i;

	out = g_ptr_array_new_with_free_func((GDestroyNotify)hk_action_free);
	if (node == NULL)
		return out;
	if (is_sequence(node)) {
		YamlSequence *seq = yaml_node_get_sequence(node);

		for (i = 0; i < yaml_sequence_get_length(seq); i++) {
			g_autofree gchar *w = g_strdup_printf("%s, action %u", where,
			                                      i + 1);
			g_autoptr(GPtrArray) one = NULL;
			guint j;

			one = hk_parse_actions(yaml_sequence_get_element(seq, i), w,
			                       problems);
			for (j = 0; j < one->len; j++)
				g_ptr_array_add(out, g_ptr_array_index(one, j));
			g_ptr_array_set_free_func(one, NULL);
		}
		return out;
	}
	if (is_mapping(node)) {
		HkAction *a = parse_action_mapping(yaml_node_get_mapping(node), where,
		                                   problems, FALSE);

		if (a != NULL)
			g_ptr_array_add(out, a);
		return out;
	}
	if (node_scalar(node) != NULL) {
		HkAction *a = g_new0(HkAction, 1);

		a->kind = HK_ACT_COMMAND;
		a->text = g_strdup(node_scalar(node));
		a->send_mode = -1;
		a->level = -1;
		a->timeout_ms = HK_DEFAULT_TIMEOUT_MS;
		a->params = json_object_new();
		g_ptr_array_add(out, a);
	}
	return out;
}

/* ── Conditions ────────────────────────────────────────────────────── */

void
hk_condition_clear(HkCondition *cond)
{
	g_clear_pointer(&cond->when_text, g_free);
	g_clear_pointer(&cond->when, gowl_filter_free);
	g_clear_pointer(&cond->over_text, g_free);
	g_clear_pointer(&cond->over, gowl_filter_free);
	g_clear_pointer(&cond->exists_text, g_free);
	g_clear_pointer(&cond->exists, gowl_filter_free);
	g_clear_pointer(&cond->absent_text, g_free);
	g_clear_pointer(&cond->absent, gowl_filter_free);
}

gboolean
hk_condition_is_empty(const HkCondition *cond)
{
	return cond->when == NULL && cond->over == NULL && cond->exists == NULL
	       && cond->absent == NULL;
}

static gboolean
compile_one(
	const gchar  *text,
	gchar       **text_out,
	GowlFilter  **filter_out,
	const gchar  *what,
	const gchar  *where,
	GPtrArray    *problems
){
	GError *e = NULL;

	if (text == NULL)
		return TRUE;
	*filter_out = gowl_filter_parse(text, hk_window_fields(), &e);
	if (*filter_out == NULL) {
		problem(problems, "%s: %s `%s': %s", where, what, text, e->message);
		g_error_free(e);
		return FALSE;
	}
	*text_out = g_strdup(text);
	return TRUE;
}

/* The condition keys of @m (`when'/`filter', `over', `exists',
 * `absent') into @cond.  FALSE when one does not compile. */
static gboolean
parse_condition(
	YamlMapping *m,
	HkCondition *cond,
	const gchar *where,
	GPtrArray   *problems
){
	const gchar *when = node_scalar(member(m, "when"));

	if (when == NULL)
		when = node_scalar(member(m, "filter"));
	return compile_one(when, &cond->when_text, &cond->when, "when", where,
	                   problems)
	       && compile_one(node_scalar(member(m, "over")), &cond->over_text,
	                      &cond->over, "over", where, problems)
	       && compile_one(node_scalar(member(m, "exists")), &cond->exists_text,
	                      &cond->exists, "exists", where, problems)
	       && compile_one(node_scalar(member(m, "absent")), &cond->absent_text,
	                      &cond->absent, "absent", where, problems);
}

static void
named_condition_free(gpointer p)
{
	HkNamedCondition *nc = p;

	g_free(nc->name);
	hk_condition_clear(&nc->cond);
	g_free(nc->origin);
	g_free(nc);
}

/* ── Hotkeys ───────────────────────────────────────────────────────── */

HkHotkey *
hk_hotkey_new(void)
{
	HkHotkey *hk = g_new0(HkHotkey, 1);

	hk->ref = 1;
	hk->enabled = TRUE;
	hk->max_running = 1;
	hk->actions = g_ptr_array_new_with_free_func(
		(GDestroyNotify)hk_action_free);
	return hk;
}

HkHotkey *
hk_hotkey_ref(HkHotkey *hk)
{
	g_return_val_if_fail(hk != NULL, NULL);
	g_atomic_int_inc(&hk->ref);
	return hk;
}

void
hk_hotkey_unref(HkHotkey *hk)
{
	if (hk == NULL || !g_atomic_int_dec_and_test(&hk->ref))
		return;
	g_free(hk->id);
	g_free(hk->key);
	g_free(hk->origin);
	g_free(hk->context);
	hk_condition_clear(&hk->cond);
	g_free(hk->mode);
	g_free(hk->description);
	g_ptr_array_unref(hk->actions);
	g_free(hk);
}

static const gchar * const hotkey_keys[] = {
	"key", "id", "do", "actions", "context", "when", "filter", "over",
	"exists", "absent", "mode", "double", "hold", "priority", "level",
	"suspend-exempt", "max-running", "enabled", "description", "desc",
	NULL
};

/* An action key used straight on a hotkey entry -- the shorthand
 * `{key: "#t", action: spawn, arg: gst}' -- but never `mode', which on
 * an entry is the hotkey's own mode. */
static gboolean
has_shorthand_action(YamlMapping *m)
{
	guint i;

	for (i = 0; action_keys[i].key != NULL; i++)
		if (action_keys[i].kind != HK_ACT_MODE
		    && action_keys[i].kind != HK_ACT_CONTEXT
		    && member(m, action_keys[i].key) != NULL)
			return TRUE;
	return FALSE;
}

/* Parses `double:'/`hold:' -- true for the default, or a duration. */
static gboolean
parse_timing(
	const gchar *text,
	guint        def,
	guint       *out
){
	gboolean b;

	if (parse_bool(text, &b)) {
		*out = b ? def : 0;
		return TRUE;
	}
	return parse_duration(text, out) && *out > 0;
}

struct _HkLoader {
	HkSet      *set;
	guint       order;
	GHashTable *seen_files;   /* include loops */
	guint       depth;
};

/* Puts @hk in the set; a hotkey of the same id is replaced (a later
 * source wins, as a later gabbrev match does). */
static void
add_hotkey(
	HkLoader *loader,
	HkHotkey *hk
){
	guint i;

	hk->order = ++loader->order;
	for (i = 0; i < loader->set->hotkeys->len; i++) {
		HkHotkey *old = g_ptr_array_index(loader->set->hotkeys, i);

		if (g_strcmp0(old->id, hk->id) == 0) {
			g_ptr_array_remove_index(loader->set->hotkeys, i);
			break;
		}
	}
	g_ptr_array_add(loader->set->hotkeys, hk);
}

/* The default id: the key as written, plus where it applies. */
static gchar *
default_id(
	const gchar *key,
	const gchar *context,
	const gchar *mode
){
	return g_strdup_printf("%s%s%s%s%s", key,
	                       context != NULL ? "@" : "",
	                       context != NULL ? context : "",
	                       mode != NULL ? "#" : "",
	                       mode != NULL ? mode : "");
}

static void
parse_hotkey(
	HkLoader    *loader,
	YamlNode    *node,
	const gchar *mode,
	const gchar *origin,
	guint        index
){
	GPtrArray *problems = loader->set->problems;
	g_autofree gchar *where = NULL;
	g_autoptr(HkHotkey) hk = NULL;
	YamlMapping *m;
	const gchar *key;
	const gchar *s;
	GError *e = NULL;
	gint64 n;

	where = g_strdup_printf("%s: hotkey %u%s%s", origin, index + 1,
	                        mode != NULL ? " in mode " : "",
	                        mode != NULL ? mode : "");
	if (!is_mapping(node)) {
		problem(problems, "%s: a hotkey is a mapping with key: and do:",
		        where);
		return;
	}
	m = yaml_node_get_mapping(node);
	key = node_scalar(member(m, "key"));
	if (key == NULL) {
		problem(problems, "%s: no key:", where);
		return;
	}
	g_free(where);
	where = g_strdup_printf("%s: hotkey `%s'%s%s", origin, key,
	                        mode != NULL ? " in mode " : "",
	                        mode != NULL ? mode : "");

	hk = hk_hotkey_new();
	hk->key = g_strdup(key);
	hk->origin = g_strdup(origin);
	if (!gowl_hotkey_spec_parse(key, &hk->spec, &e)) {
		problem(problems, "%s: %s", where, e->message);
		g_error_free(e);
		return;
	}

	/* The keys an entry may have: its own, plus a shorthand action */
	if (member(m, "do") == NULL && member(m, "actions") == NULL
	    && has_shorthand_action(m)) {
		g_autoptr(GPtrArray) known = g_ptr_array_new();
		guint i;

		for (i = 0; hotkey_keys[i] != NULL; i++)
			g_ptr_array_add(known, (gpointer)hotkey_keys[i]);
		for (i = 0; action_keys[i].key != NULL; i++)
			if (action_keys[i].kind != HK_ACT_MODE
			    && action_keys[i].kind != HK_ACT_CONTEXT)
				g_ptr_array_add(known, (gpointer)action_keys[i].key);
		for (i = 0; action_options[i] != NULL; i++)
			g_ptr_array_add(known, (gpointer)action_options[i]);
		g_ptr_array_add(known, NULL);
		check_keys(m, (const gchar * const *)known->pdata, where, problems);
		g_ptr_array_unref(hk->actions);
		{
			/* the entry itself is the action, minus its hotkey keys */
			HkAction *a = parse_action_mapping(m, where, problems, TRUE);

			hk->actions = g_ptr_array_new_with_free_func(
				(GDestroyNotify)hk_action_free);
			if (a == NULL)
				return;
			g_ptr_array_add(hk->actions, a);
		}
	} else {
		check_keys(m, hotkey_keys, where, problems);
		g_ptr_array_unref(hk->actions);
		hk->actions = hk_parse_actions(member(m, "do") != NULL
		                               ? member(m, "do")
		                               : member(m, "actions"),
		                               where, problems);
	}

	hk->context = g_strdup(node_scalar(member(m, "context")));
	if (!parse_condition(m, &hk->cond, where, problems))
		return;
	if (mode != NULL)
		hk->mode = g_strdup(mode);
	else if ((s = node_scalar(member(m, "mode"))) != NULL
	         && g_ascii_strcasecmp(s, "default") != 0)
		hk->mode = g_strdup(s);
	if ((s = node_scalar(member(m, "double"))) != NULL
	    && !parse_timing(s, HK_DEFAULT_DOUBLE_MS, &hk->double_ms)) {
		problem(problems, "%s: double is true or a duration", where);
		return;
	}
	if ((s = node_scalar(member(m, "hold"))) != NULL
	    && !parse_timing(s, HK_DEFAULT_HOLD_MS, &hk->hold_ms)) {
		problem(problems, "%s: hold is true or a duration", where);
		return;
	}
	if (hk->double_ms > 0 && hk->hold_ms > 0) {
		problem(problems, "%s: a hotkey is a double tap or a long press, "
		        "not both (make two hotkeys)", where);
		return;
	}
	if ((hk->double_ms > 0 || hk->hold_ms > 0)
	    && hk->spec.trigger.kind == GOWL_HOTKEY_INPUT_WHEEL) {
		problem(problems, "%s: a wheel notch cannot be held or "
		        "double-tapped", where);
		return;
	}
	if ((s = node_scalar(member(m, "priority"))) != NULL) {
		if (!parse_int(s, -1000, 1000, &n)) {
			problem(problems, "%s: priority is -1000..1000", where);
			return;
		}
		hk->priority = (gint)n;
	}
	if ((s = node_scalar(member(m, "level"))) != NULL) {
		if (!parse_int(s, 0, HK_MAX_LEVEL, &n)) {
			problem(problems, "%s: level is 0..%d", where, HK_MAX_LEVEL);
			return;
		}
		hk->level = (gint)n;
	}
	if ((s = node_scalar(member(m, "max-running"))) != NULL) {
		if (!parse_int(s, 0, 100, &n)) {
			problem(problems, "%s: max-running is 0 (no limit)..100", where);
			return;
		}
		hk->max_running = (guint)n;
	}
	parse_bool(node_scalar(member(m, "suspend-exempt")), &hk->suspend_exempt);
	if ((s = node_scalar(member(m, "enabled"))) != NULL)
		parse_bool(s, &hk->enabled);
	hk->description = g_strdup(node_scalar(member(m, "description")) != NULL
	                           ? node_scalar(member(m, "description"))
	                           : node_scalar(member(m, "desc")));
	if (hk->actions->len == 0) {
		problem(problems, "%s: nothing to do (do: is empty or every action "
		        "in it has a problem)", where);
		return;
	}
	hk->id = (s = node_scalar(member(m, "id"))) != NULL
	         ? g_strdup(s) : default_id(key, hk->context, hk->mode);
	add_hotkey(loader, g_steal_pointer(&hk));
}

/* ── Remaps ────────────────────────────────────────────────────────── */

static const gchar * const remap_keys[] = {
	"from", "to", "context", "when", "filter", "over", "exists", "absent",
	"mode", "priority", "suspend-exempt", "enabled", "id", "description",
	NULL
};

/*
 * One remap, `FROM: TO' -- AutoHotkey's `a::b'.  The source is a hotkey
 * spec made a wildcard (a remap carries every modifier held through, as
 * AutoHotkey's {Blind} does); the target is a key or a button, with
 * modifier symbols of its own if it has any (`CapsLock: ^Esc').
 */
static void
parse_remap(
	HkLoader    *loader,
	const gchar *from,
	const gchar *to,
	YamlMapping *opts,
	const gchar *mode,
	const gchar *origin
){
	GPtrArray *problems = loader->set->problems;
	g_autofree gchar *where = NULL;
	g_autoptr(HkHotkey) hk = NULL;
	GowlHotkeySpec target;
	GError *e = NULL;
	const gchar *s;
	gint64 n;

	where = g_strdup_printf("%s: remap `%s'", origin, from != NULL ? from : "?");
	if (from == NULL || to == NULL) {
		problem(problems, "%s: a remap needs from: and to:", where);
		return;
	}
	hk = hk_hotkey_new();
	hk->key = g_strdup(from);
	hk->origin = g_strdup(origin);
	hk->is_remap = TRUE;
	if (!gowl_hotkey_spec_parse(from, &hk->spec, &e)) {
		problem(problems, "%s: %s", where, e->message);
		g_error_free(e);
		return;
	}
	if (hk->spec.prefix.kind != GOWL_HOTKEY_INPUT_NONE
	    || hk->spec.trigger.kind == GOWL_HOTKEY_INPUT_WHEEL) {
		problem(problems, "%s: a remap is one key or button onto another "
		        "(no combinations, no wheel)", where);
		return;
	}
	hk->spec.flags |= GOWL_HOTKEY_FLAG_WILDCARD;
	if (hk->spec.trigger.kind == GOWL_HOTKEY_INPUT_KEY
	    && gowl_hotkey_keysym_modifier(hk->spec.trigger.keysym) != 0)
		/* Loaded anyway, but said: the keyboard's own state still has
		 * the source modifier down -- only a claimed device (the
		 * inputremap module) can take a modifier away */
		problem(problems, "%s: the source is a modifier key; it still acts "
		        "as that modifier too -- for a clean modifier swap use the "
		        "inputremap module or an xkb option", where);
	if (!gowl_hotkey_spec_parse(to, &target, &e)) {
		problem(problems, "%s: to `%s': %s", where, to, e->message);
		g_error_free(e);
		return;
	}
	if (target.prefix.kind != GOWL_HOTKEY_INPUT_NONE
	    || target.trigger.kind == GOWL_HOTKEY_INPUT_WHEEL) {
		problem(problems, "%s: a remap target is one key or button", where);
		return;
	}
	hk->remap_to = target.trigger;
	hk->remap_mods = target.mods;
	if (opts != NULL) {
		check_keys(opts, remap_keys, where, problems);
		hk->context = g_strdup(node_scalar(member(opts, "context")));
		if (!parse_condition(opts, &hk->cond, where, problems))
			return;
		if ((s = node_scalar(member(opts, "mode"))) != NULL && mode == NULL
		    && g_ascii_strcasecmp(s, "default") != 0)
			hk->mode = g_strdup(s);
		if ((s = node_scalar(member(opts, "priority"))) != NULL
		    && parse_int(s, -1000, 1000, &n))
			hk->priority = (gint)n;
		parse_bool(node_scalar(member(opts, "suspend-exempt")),
		           &hk->suspend_exempt);
		if ((s = node_scalar(member(opts, "enabled"))) != NULL)
			parse_bool(s, &hk->enabled);
		hk->description = g_strdup(node_scalar(member(opts, "description")));
	}
	if (mode != NULL)
		hk->mode = g_strdup(mode);
	hk->max_running = 0;
	hk->id = opts != NULL && (s = node_scalar(member(opts, "id"))) != NULL
	         ? g_strdup(s) : g_strdup_printf("remap:%s", from);
	add_hotkey(loader, g_steal_pointer(&hk));
}

static void
parse_remaps(
	HkLoader    *loader,
	YamlNode    *node,
	const gchar *mode,
	const gchar *origin
){
	guint i;

	if (is_mapping(node)) {
		YamlMapping *m = yaml_node_get_mapping(node);

		for (i = 0; i < yaml_mapping_get_size(m); i++)
			parse_remap(loader, yaml_mapping_get_key(m, i),
			            node_scalar(yaml_mapping_get_value(m, i)), NULL, mode,
			            origin);
	} else if (is_sequence(node)) {
		YamlSequence *seq = yaml_node_get_sequence(node);

		for (i = 0; i < yaml_sequence_get_length(seq); i++) {
			YamlNode *el = yaml_sequence_get_element(seq, i);
			YamlMapping *m;

			if (!is_mapping(el)) {
				problem(loader->set->problems, "%s: remap %u is a mapping "
				        "of from: and to:", origin, i + 1);
				continue;
			}
			m = yaml_node_get_mapping(el);
			parse_remap(loader, node_scalar(member(m, "from")),
			            node_scalar(member(m, "to")), m, mode, origin);
		}
	} else {
		problem(loader->set->problems, "%s: remaps: is {FROM: TO} or a list "
		        "of {from:, to:}", origin);
	}
}

/* ── Sequences of hotkeys ──────────────────────────────────────────── */

static void
parse_hotkeys(
	HkLoader    *loader,
	YamlNode    *node,
	const gchar *mode,
	const gchar *origin
){
	YamlSequence *seq;
	guint i;

	if (is_mapping(node)) {
		/* {KEY: ACTIONS} -- the compact form */
		YamlMapping *m = yaml_node_get_mapping(node);

		for (i = 0; i < yaml_mapping_get_size(m); i++) {
			g_autoptr(YamlNode) entry = yaml_node_new_mapping(NULL);
			YamlMapping *em = yaml_node_get_mapping(entry);
			YamlNode *v = yaml_mapping_get_value(m, i);

			yaml_mapping_set_string_member(em, "key",
			                               yaml_mapping_get_key(m, i));
			yaml_mapping_set_member(em, "do", v);
			parse_hotkey(loader, entry, mode, origin, i);
		}
		return;
	}
	if (!is_sequence(node)) {
		problem(loader->set->problems, "%s: hotkeys: is a list of hotkeys "
		        "(or {KEY: ACTIONS})", origin);
		return;
	}
	seq = yaml_node_get_sequence(node);
	for (i = 0; i < yaml_sequence_get_length(seq); i++)
		parse_hotkey(loader, yaml_sequence_get_element(seq, i), mode, origin,
		             i);
}

/* ── Modes ─────────────────────────────────────────────────────────── */

static void
mode_free(gpointer p)
{
	HkMode *md = p;

	g_free(md->name);
	g_free(md->label);
	if (md->on_enter != NULL)
		g_ptr_array_unref(md->on_enter);
	if (md->on_exit != NULL)
		g_ptr_array_unref(md->on_exit);
	g_free(md);
}

static const gchar * const mode_keys[] = {
	"timeout", "oneshot", "swallow", "toast", "label", "hotkeys", "remaps",
	"on-enter", "on-exit", "description", NULL
};

static void
parse_modes(
	HkLoader    *loader,
	YamlNode    *node,
	const gchar *origin
){
	GPtrArray *problems = loader->set->problems;
	YamlMapping *m;
	guint i;

	if (!is_mapping(node)) {
		problem(problems, "%s: modes: is {NAME: {...}}", origin);
		return;
	}
	m = yaml_node_get_mapping(node);
	for (i = 0; i < yaml_mapping_get_size(m); i++) {
		const gchar *name = yaml_mapping_get_key(m, i);
		YamlNode *v = yaml_mapping_get_value(m, i);
		g_autofree gchar *where = g_strdup_printf("%s: mode %s", origin, name);
		HkMode *md;
		YamlMapping *mm;
		const gchar *s;

		if (g_ascii_strcasecmp(name, "default") == 0 || strchr(name, ' ')) {
			problem(problems, "%s: a mode name is one word, not `default'",
			        where);
			continue;
		}
		md = g_new0(HkMode, 1);
		md->name = g_strdup(name);
		md->toast = TRUE;
		if (v != NULL && is_mapping(v)) {
			mm = yaml_node_get_mapping(v);
			check_keys(mm, mode_keys, where, problems);
			if ((s = node_scalar(member(mm, "timeout"))) != NULL
			    && !parse_duration(s, &md->timeout_ms))
				problem(problems, "%s: timeout `%s' is not a duration",
				        where, s);
			parse_bool(node_scalar(member(mm, "oneshot")), &md->oneshot);
			parse_bool(node_scalar(member(mm, "swallow")), &md->swallow);
			if ((s = node_scalar(member(mm, "toast"))) != NULL)
				parse_bool(s, &md->toast);
			md->label = g_strdup(node_scalar(member(mm, "label")));
			md->on_enter = hk_parse_actions(member(mm, "on-enter"),
			                                where, problems);
			md->on_exit = hk_parse_actions(member(mm, "on-exit"),
			                               where, problems);
			if (member(mm, "hotkeys") != NULL)
				parse_hotkeys(loader, member(mm, "hotkeys"), name, origin);
			if (member(mm, "remaps") != NULL)
				parse_remaps(loader, member(mm, "remaps"), name, origin);
		} else {
			md->on_enter = hk_parse_actions(NULL, where, problems);
			md->on_exit = hk_parse_actions(NULL, where, problems);
		}
		g_hash_table_replace(loader->set->modes, g_strdup(name), md);
	}
}

/* ── Contexts and groups ───────────────────────────────────────────── */

static void
parse_named_conditions(
	HkLoader    *loader,
	YamlNode    *node,
	GHashTable  *into,
	const gchar *what,
	const gchar *origin
){
	GPtrArray *problems = loader->set->problems;
	YamlMapping *m;
	guint i;

	if (!is_mapping(node)) {
		problem(problems, "%s: %s: is {NAME: FILTER}", origin, what);
		return;
	}
	m = yaml_node_get_mapping(node);
	for (i = 0; i < yaml_mapping_get_size(m); i++) {
		const gchar *name = yaml_mapping_get_key(m, i);
		YamlNode *v = yaml_mapping_get_value(m, i);
		g_autofree gchar *where = g_strdup_printf("%s: %s %s", origin, what,
		                                          name);
		HkNamedCondition *nc = g_new0(HkNamedCondition, 1);
		gboolean ok;

		nc->name = g_strdup(name);
		nc->origin = g_strdup(origin);
		if (node_scalar(v) != NULL) {
			ok = compile_one(node_scalar(v), &nc->cond.when_text, &nc->cond.when,
			                 "filter", where, problems);
		} else if (is_mapping(v)) {
			static const gchar * const keys[] = {
				"when", "filter", "over", "exists", "absent", NULL
			};

			check_keys(yaml_node_get_mapping(v), keys, where, problems);
			ok = parse_condition(yaml_node_get_mapping(v), &nc->cond, where,
			                     problems);
		} else {
			problem(problems, "%s: a filter, or {when:, over:, exists:, "
			        "absent:}", where);
			ok = FALSE;
		}
		if (!ok || hk_condition_is_empty(&nc->cond)) {
			if (ok)
				problem(problems, "%s: says nothing", where);
			named_condition_free(nc);
			continue;
		}
		g_hash_table_replace(into, g_strdup(name), nc);
	}
}

/* ── Hotstrings ────────────────────────────────────────────────────── */

static void
hotstring_free(gpointer p)
{
	HkHotstring *hs = p;

	g_free(hs->id);
	g_free(hs->raw);
	g_free(hs->options);
	g_free(hs->abbrev);
	g_free(hs->replacement);
	g_free(hs->context);
	hk_condition_clear(&hs->cond);
	if (hs->actions != NULL)
		g_ptr_array_unref(hs->actions);
	g_free(hs->origin);
	g_free(hs);
}

/* `:opts:abbr::replacement' into its three parts. */
static gboolean
split_hotstring(
	const gchar  *raw,
	gchar       **options,
	gchar       **abbrev,
	gchar       **replacement
){
	const gchar *p;
	const gchar *q;

	if (raw == NULL || raw[0] != ':')
		return FALSE;
	p = strchr(raw + 1, ':');
	if (p == NULL)
		return FALSE;
	q = strstr(p + 1, "::");
	if (q == NULL || q == p + 1)
		return FALSE;
	*options = g_strndup(raw + 1, (gsize)(p - raw - 1));
	*abbrev = g_strndup(p + 1, (gsize)(q - p - 1));
	*replacement = g_strdup(q + 2);
	return TRUE;
}

static void
parse_hotstrings(
	HkLoader    *loader,
	YamlNode    *node,
	const gchar *origin
){
	static const gchar * const keys[] = {
		"hotstring", "trigger", "replace", "options", "do", "id", "context",
		"when", "filter", "over", "exists", "absent", NULL
	};
	GPtrArray *problems = loader->set->problems;
	YamlSequence *seq;
	guint i;

	if (!is_sequence(node)) {
		problem(problems, "%s: hotstrings: is a list", origin);
		return;
	}
	seq = yaml_node_get_sequence(node);
	for (i = 0; i < yaml_sequence_get_length(seq); i++) {
		YamlNode *el = yaml_sequence_get_element(seq, i);
		g_autofree gchar *where = g_strdup_printf("%s: hotstring %u", origin,
		                                          i + 1);
		HkHotstring *hs = g_new0(HkHotstring, 1);
		const gchar *raw = node_scalar(el);
		YamlMapping *m = NULL;

		hs->origin = g_strdup(origin);
		if (raw == NULL && is_mapping(el)) {
			m = yaml_node_get_mapping(el);
			check_keys(m, keys, where, problems);
			raw = node_scalar(member(m, "hotstring"));
		}
		if (raw != NULL) {
			hs->raw = g_strdup(raw);
			if (!split_hotstring(raw, &hs->options, &hs->abbrev,
			                     &hs->replacement)) {
				problem(problems, "%s: `%s' is not :OPTIONS:ABBREVIATION::"
				        "REPLACEMENT", where, raw);
				hotstring_free(hs);
				continue;
			}
		} else if (m != NULL && node_scalar(member(m, "trigger")) != NULL) {
			hs->abbrev = g_strdup(node_scalar(member(m, "trigger")));
			hs->replacement = g_strdup(node_scalar(member(m, "replace")) != NULL
			                           ? node_scalar(member(m, "replace")) : "");
			hs->options = g_strdup(node_scalar(member(m, "options")) != NULL
			                       ? node_scalar(member(m, "options")) : "");
			hs->raw = g_strdup_printf(":%s:%s::%s", hs->options, hs->abbrev,
			                          hs->replacement);
		} else {
			problem(problems, "%s: a hotstring is \":OPTIONS:ABBR::TEXT\" or "
			        "{hotstring: ..., do: ...} or {trigger:, replace:}",
			        where);
			hotstring_free(hs);
			continue;
		}
		if (*hs->abbrev == '\0') {
			problem(problems, "%s: an empty abbreviation", where);
			hotstring_free(hs);
			continue;
		}
		if (m != NULL) {
			hs->context = g_strdup(node_scalar(member(m, "context")));
			if (!parse_condition(m, &hs->cond, where, problems)) {
				hotstring_free(hs);
				continue;
			}
			if (member(m, "do") != NULL)
				hs->actions = hk_parse_actions(member(m, "do"), where,
				                               problems);
			if (node_scalar(member(m, "id")) != NULL)
				hs->id = g_strdup(node_scalar(member(m, "id")));
		}
		if (hs->id == NULL)
			hs->id = g_strdup_printf("hotstring:%s", hs->abbrev);
		g_ptr_array_add(loader->set->hotstrings, hs);
	}
}

/* ── Timers ────────────────────────────────────────────────────────── */

static void
timer_free(gpointer p)
{
	HkTimer *t = p;

	g_free(t->id);
	g_free(t->context);
	hk_condition_clear(&t->cond);
	if (t->actions != NULL)
		g_ptr_array_unref(t->actions);
	g_free(t->origin);
	g_free(t);
}

static void
parse_timers(
	HkLoader    *loader,
	YamlNode    *node,
	const gchar *origin
){
	static const gchar * const keys[] = {
		"id", "every", "once", "delay", "enabled", "do", "context", "when",
		"filter", "over", "exists", "absent", "description", NULL
	};
	GPtrArray *problems = loader->set->problems;
	YamlSequence *seq;
	guint i;

	if (!is_sequence(node)) {
		problem(problems, "%s: timers: is a list", origin);
		return;
	}
	seq = yaml_node_get_sequence(node);
	for (i = 0; i < yaml_sequence_get_length(seq); i++) {
		YamlNode *el = yaml_sequence_get_element(seq, i);
		g_autofree gchar *where = g_strdup_printf("%s: timer %u", origin, i + 1);
		HkTimer *t;
		YamlMapping *m;
		const gchar *s;

		if (!is_mapping(el)) {
			problem(problems, "%s: a timer is {every: DURATION, do: ...}",
			        where);
			continue;
		}
		m = yaml_node_get_mapping(el);
		check_keys(m, keys, where, problems);
		t = g_new0(HkTimer, 1);
		t->enabled = TRUE;
		t->origin = g_strdup(origin);
		if ((s = node_scalar(member(m, "every"))) != NULL) {
			if (!parse_duration(s, &t->period_ms) || t->period_ms < 10) {
				problem(problems, "%s: every is a duration of 10ms or more",
				        where);
				timer_free(t);
				continue;
			}
		} else if ((s = node_scalar(member(m, "once"))) != NULL) {
			if (!parse_duration(s, &t->period_ms)) {
				problem(problems, "%s: once is a duration", where);
				timer_free(t);
				continue;
			}
			t->once = TRUE;
		} else {
			problem(problems, "%s: a timer needs every: or once:", where);
			timer_free(t);
			continue;
		}
		t->delay_ms = t->period_ms;
		if ((s = node_scalar(member(m, "delay"))) != NULL
		    && !parse_duration(s, &t->delay_ms))
			problem(problems, "%s: delay is a duration", where);
		if ((s = node_scalar(member(m, "enabled"))) != NULL)
			parse_bool(s, &t->enabled);
		t->context = g_strdup(node_scalar(member(m, "context")));
		if (!parse_condition(m, &t->cond, where, problems)) {
			timer_free(t);
			continue;
		}
		t->actions = hk_parse_actions(member(m, "do"), where, problems);
		if (t->actions->len == 0) {
			problem(problems, "%s: nothing to do", where);
			timer_free(t);
			continue;
		}
		t->id = node_scalar(member(m, "id")) != NULL
		        ? g_strdup(node_scalar(member(m, "id")))
		        : g_strdup_printf("timer:%s:%u", origin, i + 1);
		g_ptr_array_add(loader->set->timers, t);
	}
}

/* ── Event hooks ───────────────────────────────────────────────────── */

static const gchar * const hook_events[] = {
	"focus", "window-open", "window-close", "title", "tag", "monitor",
	"mode", "startup", "lock", "unlock", NULL
};

static void
hook_free(gpointer p)
{
	HkHook *h = p;

	g_free(h->id);
	g_free(h->event);
	hk_condition_clear(&h->cond);
	if (h->actions != NULL)
		g_ptr_array_unref(h->actions);
	g_free(h->origin);
	g_free(h);
}

static void
add_hook(
	HkLoader    *loader,
	const gchar *event,
	YamlNode    *node,
	guint        index,
	const gchar *origin
){
	static const gchar * const keys[] = {
		"id", "do", "when", "filter", "over", "exists", "absent",
		"description", NULL
	};
	g_autofree gchar *where = g_strdup_printf("%s: on %s %u", origin, event,
	                                          index + 1);
	GPtrArray *problems = loader->set->problems;
	HkHook *h = g_new0(HkHook, 1);

	h->event = g_strdup(event);
	h->origin = g_strdup(origin);
	if (is_mapping(node) && member(yaml_node_get_mapping(node), "do") != NULL) {
		YamlMapping *m = yaml_node_get_mapping(node);

		check_keys(m, keys, where, problems);
		if (!parse_condition(m, &h->cond, where, problems)) {
			hook_free(h);
			return;
		}
		h->actions = hk_parse_actions(member(m, "do"), where, problems);
		if (node_scalar(member(m, "id")) != NULL)
			h->id = g_strdup(node_scalar(member(m, "id")));
	} else {
		h->actions = hk_parse_actions(node, where, problems);
	}
	if (h->actions->len == 0) {
		problem(problems, "%s: nothing to do", where);
		hook_free(h);
		return;
	}
	if (h->id == NULL)
		h->id = g_strdup_printf("on:%s:%u", event,
		                        loader->set->hooks->len + 1);
	g_ptr_array_add(loader->set->hooks, h);
}

static void
parse_hooks(
	HkLoader    *loader,
	YamlNode    *node,
	const gchar *origin
){
	GPtrArray *problems = loader->set->problems;
	YamlMapping *m;
	guint i;
	guint j;

	if (!is_mapping(node)) {
		problem(problems, "%s: on: is {EVENT: ...}", origin);
		return;
	}
	m = yaml_node_get_mapping(node);
	for (i = 0; i < yaml_mapping_get_size(m); i++) {
		const gchar *event = yaml_mapping_get_key(m, i);
		YamlNode *v = yaml_mapping_get_value(m, i);
		gboolean known = FALSE;

		for (j = 0; hook_events[j] != NULL; j++)
			if (g_strcmp0(hook_events[j], event) == 0)
				known = TRUE;
		if (!known) {
			problem(problems, "%s: on: unknown event `%s' (focus, "
			        "window-open, window-close, title, tag, monitor, mode, "
			        "startup, lock, unlock)", origin, event);
			continue;
		}
		/* A list of hooks ({do:} each), or one hook, or an action list */
		if (is_sequence(v)) {
			YamlSequence *seq = yaml_node_get_sequence(v);
			gboolean hooks = yaml_sequence_get_length(seq) > 0;

			for (j = 0; j < yaml_sequence_get_length(seq); j++) {
				YamlNode *el = yaml_sequence_get_element(seq, j);

				if (!is_mapping(el)
				    || member(yaml_node_get_mapping(el), "do") == NULL)
					hooks = FALSE;
			}
			if (hooks)
				for (j = 0; j < yaml_sequence_get_length(seq); j++)
					add_hook(loader, event, yaml_sequence_get_element(seq, j),
					         j, origin);
			else
				add_hook(loader, event, v, 0, origin);
		} else {
			add_hook(loader, event, v, 0, origin);
		}
	}
}

/* ── Functions ─────────────────────────────────────────────────────── */

static void
function_free(gpointer p)
{
	HkFunction *f = p;

	g_free(f->name);
	if (f->actions != NULL)
		g_ptr_array_unref(f->actions);
	g_free(f->origin);
	g_free(f);
}

static void
parse_functions(
	HkLoader    *loader,
	YamlNode    *node,
	const gchar *origin
){
	GPtrArray *problems = loader->set->problems;
	YamlMapping *m;
	guint i;

	if (!is_mapping(node)) {
		problem(problems, "%s: functions: is {NAME: ACTIONS}", origin);
		return;
	}
	m = yaml_node_get_mapping(node);
	for (i = 0; i < yaml_mapping_get_size(m); i++) {
		const gchar *name = yaml_mapping_get_key(m, i);
		g_autofree gchar *where = g_strdup_printf("%s: function %s", origin,
		                                          name);
		HkFunction *f = g_new0(HkFunction, 1);

		f->name = g_strdup(name);
		f->origin = g_strdup(origin);
		f->actions = hk_parse_actions(yaml_mapping_get_value(m, i), where,
		                              problems);
		if (f->actions->len == 0) {
			problem(problems, "%s: nothing to do", where);
			function_free(f);
			continue;
		}
		g_hash_table_replace(loader->set->functions, g_strdup(name), f);
	}
}

/* ── The set ───────────────────────────────────────────────────────── */

static HkSet *
hk_set_new_empty(void)
{
	HkSet *set = g_new0(HkSet, 1);

	set->ref = 1;
	set->hotkeys = g_ptr_array_new_with_free_func(
		(GDestroyNotify)hk_hotkey_unref);
	set->modes = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
	                                   mode_free);
	set->contexts = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
	                                      named_condition_free);
	set->groups = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
	                                    named_condition_free);
	set->hotstrings = g_ptr_array_new_with_free_func(hotstring_free);
	set->timers = g_ptr_array_new_with_free_func(timer_free);
	set->hooks = g_ptr_array_new_with_free_func(hook_free);
	set->functions = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
	                                       function_free);
	set->scripts = g_ptr_array_new_with_free_func(g_free);
	set->problems = g_ptr_array_new_with_free_func(g_free);
	return set;
}

HkSet *
hk_set_ref(HkSet *set)
{
	g_return_val_if_fail(set != NULL, NULL);
	g_atomic_int_inc(&set->ref);
	return set;
}

void
hk_set_unref(HkSet *set)
{
	if (set == NULL || !g_atomic_int_dec_and_test(&set->ref))
		return;
	g_ptr_array_unref(set->hotkeys);
	g_hash_table_unref(set->modes);
	g_hash_table_unref(set->contexts);
	g_hash_table_unref(set->groups);
	g_ptr_array_unref(set->hotstrings);
	g_ptr_array_unref(set->timers);
	g_ptr_array_unref(set->hooks);
	g_hash_table_unref(set->functions);
	g_ptr_array_unref(set->scripts);
	g_ptr_array_unref(set->problems);
	g_free(set);
}

HkHotkey *
hk_set_find_hotkey(
	HkSet       *set,
	const gchar *id
){
	guint i;

	g_return_val_if_fail(set != NULL, NULL);
	for (i = 0; id != NULL && i < set->hotkeys->len; i++) {
		HkHotkey *hk = g_ptr_array_index(set->hotkeys, i);

		if (g_strcmp0(hk->id, id) == 0)
			return hk;
	}
	return NULL;
}

guint
hk_set_count_enabled(HkSet *set)
{
	guint i;
	guint n = 0;

	g_return_val_if_fail(set != NULL, 0);
	for (i = 0; i < set->hotkeys->len; i++)
		if (((HkHotkey *)g_ptr_array_index(set->hotkeys, i))->enabled)
			n++;
	return n;
}

/* ── The loader ────────────────────────────────────────────────────── */

HkLoader *
hk_loader_new(void)
{
	HkLoader *loader = g_new0(HkLoader, 1);

	loader->set = hk_set_new_empty();
	loader->seen_files = g_hash_table_new_full(g_str_hash, g_str_equal,
	                                           g_free, NULL);
	return loader;
}

void
hk_loader_free(HkLoader *loader)
{
	if (loader == NULL)
		return;
	if (loader->set != NULL)
		hk_set_unref(loader->set);
	g_hash_table_unref(loader->seen_files);
	g_free(loader);
}

/* `~/x' and a path relative to @base into an absolute path. */
static gchar *
resolve_path(
	const gchar *path,
	const gchar *base
){
	if (g_str_has_prefix(path, "~/"))
		return g_build_filename(g_get_home_dir(), path + 2, NULL);
	if (g_path_is_absolute(path) || base == NULL)
		return g_strdup(path);
	return g_build_filename(base, path, NULL);
}

static void load_include (HkLoader *loader, const gchar *path);

static const gchar * const section_keys[] = {
	"include", "scripts", "contexts", "groups", "hotkeys", "remaps",
	"modes", "hotstrings", "timers", "on", "functions", NULL
};

/**
 * hk_loader_add_node: (skip)
 * @loader: a loader
 * @section: a `hotkey:' section
 * @origin: what to call it in a problem (`config', a source id)
 * @base_dir: (nullable): what relative `include:' and `scripts:'
 *   paths are relative to
 */
void
hk_loader_add_node(
	HkLoader    *loader,
	YamlNode    *section,
	const gchar *origin,
	const gchar *base_dir
){
	GPtrArray *problems;
	YamlMapping *m;
	guint i;

	g_return_if_fail(loader != NULL);
	problems = loader->set->problems;
	if (!is_mapping(section)) {
		problem(problems, "%s: a hotkey section is a mapping", origin);
		return;
	}
	m = yaml_node_get_mapping(section);
	check_keys(m, section_keys, origin, problems);

	/* Contexts and groups first: hotkeys name them */
	if (member(m, "contexts") != NULL)
		parse_named_conditions(loader, member(m, "contexts"),
		                       loader->set->contexts, "contexts", origin);
	if (member(m, "groups") != NULL)
		parse_named_conditions(loader, member(m, "groups"),
		                       loader->set->groups, "groups", origin);
	if (member(m, "functions") != NULL)
		parse_functions(loader, member(m, "functions"), origin);
	if (member(m, "hotkeys") != NULL)
		parse_hotkeys(loader, member(m, "hotkeys"), NULL, origin);
	if (member(m, "remaps") != NULL)
		parse_remaps(loader, member(m, "remaps"), NULL, origin);
	if (member(m, "modes") != NULL)
		parse_modes(loader, member(m, "modes"), origin);
	if (member(m, "hotstrings") != NULL)
		parse_hotstrings(loader, member(m, "hotstrings"), origin);
	if (member(m, "timers") != NULL)
		parse_timers(loader, member(m, "timers"), origin);
	if (member(m, "on") != NULL)
		parse_hooks(loader, member(m, "on"), origin);
	if (member(m, "scripts") != NULL) {
		g_auto(GStrv) list = string_list(member(m, "scripts"));

		for (i = 0; list != NULL && list[i] != NULL; i++)
			g_ptr_array_add(loader->set->scripts,
			                resolve_path(list[i], base_dir));
	}
	if (member(m, "include") != NULL) {
		g_auto(GStrv) list = string_list(member(m, "include"));

		for (i = 0; list != NULL && list[i] != NULL; i++) {
			g_autofree gchar *path = resolve_path(list[i], base_dir);

			load_include(loader, path);
		}
	}
}

/* An `include:' file: its whole content is a section, or it has one
 * `hotkey:' key (so a gowl config file can be included as is). */
static void
load_include(
	HkLoader    *loader,
	const gchar *path
){
	g_autoptr(YamlParser) parser = NULL;
	g_autofree gchar *dir = NULL;
	GError *e = NULL;
	YamlNode *root;

	if (loader->depth >= 8) {
		problem(loader->set->problems, "%s: include: nested too deep", path);
		return;
	}
	if (g_hash_table_contains(loader->seen_files, path)) {
		problem(loader->set->problems, "%s: included twice (a loop?)", path);
		return;
	}
	g_hash_table_add(loader->seen_files, g_strdup(path));
	parser = yaml_parser_new();
	/* Always with an error location: yaml-glib dereferences a NULL one
	   on malformed input */
	if (!yaml_parser_load_from_file(parser, path, &e)) {
		problem(loader->set->problems, "%s: %s", path, e->message);
		g_error_free(e);
		return;
	}
	root = yaml_parser_get_root(parser);
	if (is_mapping(root)
	    && member(yaml_node_get_mapping(root), "hotkey") != NULL)
		root = member(yaml_node_get_mapping(root), "hotkey");
	dir = g_path_get_dirname(path);
	loader->depth++;
	hk_loader_add_node(loader, root, path, dir);
	loader->depth--;
}

/**
 * hk_loader_add_text: (skip)
 * @loader: a loader
 * @yaml: a section's text
 * @origin: what to call it
 * @error: a YAML syntax error
 *
 * Returns: %TRUE when it parsed (its problems are in the set)
 */
gboolean
hk_loader_add_text(
	HkLoader     *loader,
	const gchar  *yaml,
	const gchar  *origin,
	GError      **error
){
	g_autoptr(YamlParser) parser = NULL;
	GError *local = NULL;

	g_return_val_if_fail(loader != NULL && yaml != NULL, FALSE);
	parser = yaml_parser_new();
	if (!yaml_parser_load_from_data(parser, yaml, -1, &local)) {
		g_propagate_error(error, local);
		return FALSE;
	}
	hk_loader_add_node(loader, yaml_parser_get_root(parser), origin, NULL);
	return TRUE;
}

/**
 * hk_loader_finish: (skip)
 * @loader: a loader
 *
 * Checks what only the whole set can say -- a hotkey naming a context
 * or mode that is not there -- and hands the set over.
 *
 * Returns: (transfer full): the set
 */
HkSet *
hk_loader_finish(HkLoader *loader)
{
	HkSet *set;
	guint i;

	g_return_val_if_fail(loader != NULL, NULL);
	set = g_steal_pointer(&loader->set);
	loader->set = hk_set_new_empty();
	for (i = 0; i < set->hotkeys->len; i++) {
		HkHotkey *hk = g_ptr_array_index(set->hotkeys, i);
		guint j;

		if (hk->mode != NULL && g_strcmp0(hk->mode, "*") != 0
		    && !g_hash_table_contains(set->modes, hk->mode))
			/* A mode no `modes:' entry describes still works -- it is
			 * the compositor's key mode, which a keybind may enter -- but
			 * is worth a line in case it is a typo */
			problem(set->problems, "%s: hotkey `%s' is in mode `%s', which "
			        "modes: does not describe (fine if a keybind enters it)",
			        hk->origin, hk->key, hk->mode);
		for (j = 0; j < hk->actions->len; j++) {
			HkAction *a = g_ptr_array_index(hk->actions, j);

			if (a->kind == HK_ACT_MODE && g_strcmp0(a->name, "default") != 0
			    && !g_hash_table_contains(set->modes, a->name))
				problem(set->problems, "%s: hotkey `%s' enters mode `%s', "
				        "which modes: does not describe", hk->origin,
				        hk->key, a->name);
		}
	}
	return set;
}

/* ── JSON ──────────────────────────────────────────────────────────── */

static void
condition_to_json(
	const HkCondition *c,
	JsonBuilder       *b
){
	if (c->when_text != NULL) {
		json_builder_set_member_name(b, "when");
		json_builder_add_string_value(b, c->when_text);
	}
	if (c->over_text != NULL) {
		json_builder_set_member_name(b, "over");
		json_builder_add_string_value(b, c->over_text);
	}
	if (c->exists_text != NULL) {
		json_builder_set_member_name(b, "exists");
		json_builder_add_string_value(b, c->exists_text);
	}
	if (c->absent_text != NULL) {
		json_builder_set_member_name(b, "absent");
		json_builder_add_string_value(b, c->absent_text);
	}
}

/**
 * hk_hotkey_to_json: (skip)
 * @hk: a hotkey
 * @b: a builder, with an object open
 */
void
hk_hotkey_to_json(
	const HkHotkey *hk,
	JsonBuilder    *b
){
	g_autofree gchar *canon = gowl_hotkey_spec_to_string(&hk->spec);
	guint i;

	json_builder_begin_object(b);
	json_builder_set_member_name(b, "id");
	json_builder_add_string_value(b, hk->id);
	json_builder_set_member_name(b, "key");
	json_builder_add_string_value(b, hk->key);
	json_builder_set_member_name(b, "canonical");
	json_builder_add_string_value(b, canon);
	json_builder_set_member_name(b, "kind");
	json_builder_add_string_value(b, hk->is_remap ? "remap"
		: hk->spec.prefix.kind != GOWL_HOTKEY_INPUT_NONE ? "combination"
		: hk->double_ms > 0 ? "double"
		: hk->hold_ms > 0 ? "hold"
		: (hk->spec.flags & GOWL_HOTKEY_FLAG_ON_RELEASE) ? "up" : "press");
	json_builder_set_member_name(b, "enabled");
	json_builder_add_boolean_value(b, hk->enabled);
	json_builder_set_member_name(b, "origin");
	json_builder_add_string_value(b, hk->origin);
	if (hk->context != NULL) {
		json_builder_set_member_name(b, "context");
		json_builder_add_string_value(b, hk->context);
	}
	condition_to_json(&hk->cond, b);
	json_builder_set_member_name(b, "mode");
	json_builder_add_string_value(b, hk->mode != NULL ? hk->mode : "default");
	if (hk->double_ms > 0) {
		json_builder_set_member_name(b, "double-ms");
		json_builder_add_int_value(b, hk->double_ms);
	}
	if (hk->hold_ms > 0) {
		json_builder_set_member_name(b, "hold-ms");
		json_builder_add_int_value(b, hk->hold_ms);
	}
	json_builder_set_member_name(b, "priority");
	json_builder_add_int_value(b, hk->priority);
	json_builder_set_member_name(b, "level");
	json_builder_add_int_value(b, hk->level);
	json_builder_set_member_name(b, "suspend-exempt");
	json_builder_add_boolean_value(b, hk->suspend_exempt);
	if (hk->is_remap) {
		g_autofree gchar *to = gowl_hotkey_input_to_string(&hk->remap_to);

		json_builder_set_member_name(b, "to");
		json_builder_add_string_value(b, to);
	}
	if (hk->description != NULL) {
		json_builder_set_member_name(b, "description");
		json_builder_add_string_value(b, hk->description);
	}
	json_builder_set_member_name(b, "actions");
	json_builder_begin_array(b);
	for (i = 0; i < hk->actions->len; i++) {
		HkAction *a = g_ptr_array_index(hk->actions, i);

		json_builder_add_string_value(b, hk_action_kind_name(a->kind));
	}
	json_builder_end_array(b);
	json_builder_end_object(b);
}
