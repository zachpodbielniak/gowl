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
 * Bar slots, against the real bar module.
 *
 * A slot is a bar item whose content is pushed from outside
 * (modules/bar/bar-slots.h).  The module loads and takes commands
 * without a compositor, so everything a push can say -- and everything
 * it may not -- is asserted here through the same `bar-slot-*' commands
 * gowl-msg, cmacs and the JSON-RPC methods end in.
 *
 * What needs a compositor is NOT here: drawing, a slot limited to one
 * output, the click event and a time-to-live running out (the tick is a
 * wl_event_loop timer).
 */

#include <gmodule.h>
#include <string.h>

#include "module/gowl-module.h"
#include "interfaces/gowl-ipc-handler.h"

#ifndef GOWL_TEST_BAR_MODULE
#define GOWL_TEST_BAR_MODULE "build/release/modules/bar.so"
#endif

/* Mirrors BAR_SLOTS_MAX and BAR_SLOT_TEXT_MAX in modules/bar/bar-slots.h.
   Written out rather than included: the limits are part of what the
   commands promise, and a test that read them from the header would
   agree with any value. */
#define SLOTS_MAX (64)
#define TEXT_MAX  (256)

static GowlModule *
load_bar_module(void)
{
	static GType type = 0;
	GModule *module;
	gpointer symbol;
	GType (*register_fn)(void);

	if (type == 0) {
		module = g_module_open(GOWL_TEST_BAR_MODULE,
		                       G_MODULE_BIND_LAZY);
		if (module == NULL) {
			g_test_skip(g_module_error());
			return NULL;
		}
		g_module_make_resident(module);
		if (!g_module_symbol(module, "gowl_module_register", &symbol)) {
			g_test_skip("bar.so exports no gowl_module_register");
			return NULL;
		}
		register_fn = (GType (*)(void))symbol;
		type = register_fn();
	}
	return (GowlModule *)g_object_new(type, NULL);
}

static gchar *
run(GowlModule *module, const gchar *command, const gchar *args)
{
	return gowl_ipc_handler_handle_command(GOWL_IPC_HANDLER(module),
	                                       command, args);
}

/* Run a command whose reply is not what is being looked at, and check
   only that it was not refused. */
static void
run_ok(GowlModule *module, const gchar *command, const gchar *args)
{
	g_autofree gchar *reply = NULL;

	reply = run(module, command, args);
	g_assert_nonnull(reply);
	if (g_str_has_prefix(reply, "error"))
		g_error("%s %s: %s", command, (args != NULL) ? args : "", reply);
}

/* Run a command that must be refused. */
static void
run_refused(GowlModule *module, const gchar *command, const gchar *args)
{
	g_autofree gchar *reply = NULL;

	reply = run(module, command, args);
	g_assert_nonnull(reply);
	if (!g_str_has_prefix(reply, "error"))
		g_error("%s %s was accepted: %s", command,
		        (args != NULL) ? args : "", reply);
}

static void
configure(GowlModule *module, const gchar *key, const gchar *value)
{
	g_autoptr(GHashTable) settings = NULL;

	settings = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
	                                 g_free);
	g_hash_table_insert(settings, g_strdup(key), g_strdup(value));
	gowl_module_configure(module, settings);
}

/*
 * The `bar-widgets' line for a widget, or NULL; and through @index
 * which line it is.  A widget line is `  REGION SPEC [MARK] LABEL'.
 */
static gchar *
widget_line(GowlModule *module, const gchar *spec, gint *index)
{
	g_autofree gchar *listing = NULL;
	g_auto(GStrv) lines = NULL;
	gint i;

	if (index != NULL)
		*index = -1;
	listing = run(module, "bar-widgets", NULL);
	g_assert_nonnull(listing);

	lines = g_strsplit(listing, "\n", -1);
	for (i = 0; lines[i] != NULL; i++) {
		g_auto(GStrv) fields = NULL;
		gint f;

		if (lines[i][0] != ' ')
			continue;
		fields = g_strsplit_set(lines[i], " \t", -1);
		for (f = 0; fields[f] != NULL; f++) {
			if (g_strcmp0(fields[f], spec) == 0) {
				if (index != NULL)
					*index = i;
				return g_strdup(lines[i]);
			}
		}
	}
	return NULL;
}

static gint
widget_index(GowlModule *module, const gchar *spec)
{
	g_autofree gchar *line = NULL;
	gint index;

	line = widget_line(module, spec, &index);
	return index;
}

static gint
widget_count(GowlModule *module, const gchar *spec)
{
	g_autofree gchar *listing = NULL;
	g_auto(GStrv) lines = NULL;
	gint i, found;

	listing = run(module, "bar-widgets", NULL);
	lines = g_strsplit(listing, "\n", -1);
	found = 0;
	for (i = 0; lines[i] != NULL; i++) {
		g_auto(GStrv) fields = NULL;
		gint f;

		if (lines[i][0] != ' ')
			continue;
		fields = g_strsplit_set(lines[i], " \t", -1);
		for (f = 0; fields[f] != NULL; f++) {
			if (g_strcmp0(fields[f], spec) == 0) {
				found++;
				break;
			}
		}
	}
	return found;
}

/* Which line the heading of the bottom bar is on: a widget after it is
   on the bottom bar. */
static gint
bottom_heading_index(GowlModule *module)
{
	g_autofree gchar *listing = NULL;
	g_auto(GStrv) lines = NULL;
	gint i;

	listing = run(module, "bar-widgets", NULL);
	lines = g_strsplit(listing, "\n", -1);
	for (i = 0; lines[i] != NULL; i++) {
		if (g_str_has_prefix(lines[i], "bottom"))
			return i;
	}
	return -1;
}

/* ---------------------------------------------------------------- */

static void
test_a_slot_is_created_and_updated(void)
{
	GowlModule *module;
	g_autofree gchar *reply = NULL;
	g_autofree gchar *again = NULL;
	g_autofree gchar *list = NULL;
	g_autofree gchar *line = NULL;

	module = load_bar_module();
	if (module == NULL)
		return;

	/* Nothing yet. */
	list = run(module, "bar-slot-list", NULL);
	g_assert_cmpstr(list, ==, "[]\n");
	g_clear_pointer(&list, g_free);

	/* The shell form: a name, then what it says. */
	reply = run(module, "bar-slot-set", "mail 3 unread");
	g_assert_cmpstr(reply, ==, "created mail\n");

	list = run(module, "bar-slot-list", NULL);
	g_assert_nonnull(strstr(list, "\"id\":\"mail\""));
	g_assert_nonnull(strstr(list, "\"text\":\"3 unread\""));
	/* The defaults a slot gets when it says nothing else. */
	g_assert_nonnull(strstr(list, "\"bar\":\"top\""));
	g_assert_nonnull(strstr(list, "\"region\":\"right\""));
	g_assert_nonnull(strstr(list, "\"urgency\":\"normal\""));
	g_assert_nonnull(strstr(list, "\"click\":false"));
	g_assert_nonnull(strstr(list, "\"placed\":true"));

	/* It is on the bar, once, as an ordinary widget showing the text. */
	g_assert_cmpint(widget_count(module, "slot:mail"), ==, 1);
	line = widget_line(module, "slot:mail", NULL);
	g_assert_nonnull(strstr(line, "3 unread"));
	g_assert_nonnull(strstr(line, "right"));
	g_clear_pointer(&line, g_free);

	/* A second push changes it in place: one slot, one item. */
	again = run(module, "bar-slot-set", "mail 4 unread");
	g_assert_cmpstr(again, ==, "updated mail\n");
	g_assert_cmpint(widget_count(module, "slot:mail"), ==, 1);
	line = widget_line(module, "slot:mail", NULL);
	g_assert_nonnull(strstr(line, "4 unread"));

	g_object_unref(module);
}

static void
test_json_carries_every_field(void)
{
	GowlModule *module;
	g_autofree gchar *reply = NULL;
	g_autofree gchar *list = NULL;

	module = load_bar_module();
	if (module == NULL)
		return;

	reply = run(module, "bar-slot-set",
		"{\"id\":\"ci\",\"text\":\"2 failing\",\"icon\":\"!\","
		"\"role\":\"peach\",\"tooltip\":\"main, release\","
		"\"urgency\":\"critical\",\"click\":true,\"bar\":\"bottom\","
		"\"region\":\"left\",\"order\":3,\"output\":\"eDP-1\","
		"\"owner\":\"ci-watch\",\"ttl\":30}");
	g_assert_cmpstr(reply, ==, "created ci\n");

	list = run(module, "bar-slot-list", NULL);
	g_assert_nonnull(strstr(list, "\"text\":\"2 failing\""));
	g_assert_nonnull(strstr(list, "\"icon\":\"!\""));
	g_assert_nonnull(strstr(list, "\"role\":\"peach\""));
	g_assert_nonnull(strstr(list, "\"tooltip\":\"main, release\""));
	g_assert_nonnull(strstr(list, "\"urgency\":\"critical\""));
	g_assert_nonnull(strstr(list, "\"click\":true"));
	g_assert_nonnull(strstr(list, "\"bar\":\"bottom\""));
	g_assert_nonnull(strstr(list, "\"region\":\"left\""));
	g_assert_nonnull(strstr(list, "\"order\":3"));
	g_assert_nonnull(strstr(list, "\"output\":\"eDP-1\""));
	g_assert_nonnull(strstr(list, "\"owner\":\"ci-watch\""));
	g_assert_nonnull(strstr(list, "\"ttl\":30"));
	g_clear_pointer(&list, g_free);

	/* It went to the bottom bar. */
	g_assert_cmpint(widget_index(module, "slot:ci"), >,
	                bottom_heading_index(module));

	/* Only the keys present change: a text-only push keeps the rest. */
	run_ok(module, "bar-slot-set",
	       "{\"id\":\"ci\",\"owner\":\"ci-watch\",\"text\":\"ok\"}");
	list = run(module, "bar-slot-list", NULL);
	g_assert_nonnull(strstr(list, "\"text\":\"ok\""));
	g_assert_nonnull(strstr(list, "\"role\":\"peach\""));
	g_assert_nonnull(strstr(list, "\"urgency\":\"critical\""));
	g_clear_pointer(&list, g_free);

	/* An empty string, or null, clears a field. */
	run_ok(module, "bar-slot-set",
	       "{\"id\":\"ci\",\"owner\":\"ci-watch\",\"role\":\"\","
	       "\"output\":null}");
	list = run(module, "bar-slot-list", NULL);
	g_assert_nonnull(strstr(list, "\"role\":\"\""));
	g_assert_nonnull(strstr(list, "\"output\":\"\""));

	g_object_unref(module);
}

static void
test_what_a_push_may_not_say(void)
{
	GowlModule *module;
	g_autofree gchar *list = NULL;

	module = load_bar_module();
	if (module == NULL)
		return;

	/* An id is a word of a safe alphabet: it travels on an event line
	   and inside a widget spec. */
	run_refused(module, "bar-slot-set", "{\"id\":\"two words\"}");
	run_refused(module, "bar-slot-set", "{\"id\":\"a:b\"}");
	run_refused(module, "bar-slot-set", "{\"id\":\"a@5\"}");
	run_refused(module, "bar-slot-set", "{\"id\":\"\"}");
	/* Nor one that reads as an option: `bar-slot-clear --all' must
	   never be somebody's owner tag. */
	run_refused(module, "bar-slot-set", "{\"id\":\"--all\"}");
	run_refused(module, "bar-slot-set",
	            "{\"id\":\"x\",\"owner\":\"--all\"}");
	run_refused(module, "bar-slot-set", "{\"text\":\"no id\"}");
	run_refused(module, "bar-slot-set", "");
	run_refused(module, "bar-slot-set", "{\"id\":");
	run_refused(module, "bar-slot-set", "{\"id\":7}");

	/* A role is a role.  A colour the pusher chose would stay that
	   colour when the palette changed. */
	run_refused(module, "bar-slot-set",
	            "{\"id\":\"x\",\"role\":\"#ff0000\"}");
	run_refused(module, "bar-slot-set",
	            "{\"id\":\"x\",\"role\":\"crimson\"}");

	run_refused(module, "bar-slot-set",
	            "{\"id\":\"x\",\"region\":\"sideways\"}");
	run_refused(module, "bar-slot-set", "{\"id\":\"x\",\"bar\":\"side\"}");
	run_refused(module, "bar-slot-set",
	            "{\"id\":\"x\",\"urgency\":\"loud\"}");
	run_refused(module, "bar-slot-set", "{\"id\":\"x\",\"click\":\"yes\"}");
	run_refused(module, "bar-slot-set", "{\"id\":\"x\",\"order\":\"1\"}");

	/* A key that does not exist is an error, not a setting silently
	   ignored. */
	run_refused(module, "bar-slot-set",
	            "{\"id\":\"x\",\"colour\":\"red\"}");
	run_refused(module, "bar-slot-set",
	            "{\"id\":\"x\",\"command\":\"rm -rf ~\"}");

	/* Not one of those made a slot. */
	list = run(module, "bar-slot-list", NULL);
	g_assert_cmpstr(list, ==, "[]\n");
	g_assert_cmpint(widget_count(module, "slot:x"), ==, 0);
	g_clear_pointer(&list, g_free);

	/* All or nothing: one bad value and the good ones beside it are
	   not applied either. */
	run_ok(module, "bar-slot-set", "x before");
	run_refused(module, "bar-slot-set",
	            "{\"id\":\"x\",\"text\":\"after\",\"urgency\":\"loud\"}");
	list = run(module, "bar-slot-list", NULL);
	g_assert_nonnull(strstr(list, "\"text\":\"before\""));
	g_assert_null(strstr(list, "after"));

	g_object_unref(module);
}

static void
test_text_is_bounded(void)
{
	GowlModule *module;
	g_autofree gchar *list = NULL;
	g_autofree gchar *line = NULL;
	GString *args;
	const gchar *text, *end;
	gint i;

	module = load_bar_module();
	if (module == NULL)
		return;

	/* Far too long: cut, not refused. */
	args = g_string_new("long ");
	for (i = 0; i < 1000; i++)
		g_string_append_c(args, 'a');
	run_ok(module, "bar-slot-set", args->str);
	g_string_free(args, TRUE);

	list = run(module, "bar-slot-list", NULL);
	text = strstr(list, "\"text\":\"");
	g_assert_nonnull(text);
	text += strlen("\"text\":\"");
	end = strchr(text, '"');
	g_assert_nonnull(end);
	g_assert_cmpint((gint)(end - text), ==, TEXT_MAX);
	g_clear_pointer(&list, g_free);

	/* The cut never lands inside a character: 255 bytes and then a
	   two-byte one leaves 255, not 256 with half a letter. */
	args = g_string_new("edge ");
	for (i = 0; i < TEXT_MAX - 1; i++)
		g_string_append_c(args, 'a');
	g_string_append(args, "\xc3\xa9tude");
	run_ok(module, "bar-slot-set", args->str);
	g_string_free(args, TRUE);

	list = run(module, "bar-slot-list", "");
	g_assert_true(g_utf8_validate(list, -1, NULL));
	g_assert_null(strstr(list, "\xc3\xa9"));
	g_clear_pointer(&list, g_free);

	/* A bar is one line: control characters become spaces. */
	run_ok(module, "bar-slot-set",
	       "{\"id\":\"ctl\",\"text\":\"a\\nb\\tc\\u001b[31m\"}");
	line = widget_line(module, "slot:ctl", NULL);
	g_assert_nonnull(line);
	g_assert_null(strchr(line, '\x1b'));
	g_assert_nonnull(strstr(line, "a b c [31m"));

	/* Not UTF-8 at all. */
	run_refused(module, "bar-slot-set", "bad \xff\xfe");

	g_object_unref(module);
}

static void
test_slots_are_capped(void)
{
	GowlModule *module;
	gint i;

	module = load_bar_module();
	if (module == NULL)
		return;

	for (i = 0; i < SLOTS_MAX; i++) {
		g_autofree gchar *args = NULL;

		args = g_strdup_printf("s%d %d", i, i);
		run_ok(module, "bar-slot-set", args);
	}

	/* One more is refused... */
	run_refused(module, "bar-slot-set", "straw x");
	g_assert_cmpint(widget_count(module, "slot:straw"), ==, 0);

	/* ...but the ones there still take pushes... */
	run_ok(module, "bar-slot-set", "s0 still here");

	/* ...and removing one makes room. */
	run_ok(module, "bar-slot-remove", "s1");
	run_ok(module, "bar-slot-set", "straw x");
	g_assert_cmpint(widget_count(module, "slot:straw"), ==, 1);

	g_object_unref(module);
}

static void
test_an_owner_keeps_its_slots(void)
{
	GowlModule *module;
	g_autofree gchar *reply = NULL;
	g_autofree gchar *list = NULL;

	module = load_bar_module();
	if (module == NULL)
		return;

	run_ok(module, "bar-slot-set",
	       "{\"id\":\"a1\",\"owner\":\"alpha\",\"text\":\"one\"}");
	run_ok(module, "bar-slot-set",
	       "{\"id\":\"a2\",\"owner\":\"alpha\",\"text\":\"two\"}");
	run_ok(module, "bar-slot-set",
	       "{\"id\":\"b1\",\"owner\":\"beta\",\"text\":\"three\"}");
	run_ok(module, "bar-slot-set", "free four");

	/* Somebody else, and nobody in particular, may not write on it. */
	run_refused(module, "bar-slot-set",
	            "{\"id\":\"a1\",\"owner\":\"beta\",\"text\":\"mine\"}");
	run_refused(module, "bar-slot-set", "a1 mine");
	/* Nor may an owner adopt a slot that has none. */
	run_refused(module, "bar-slot-set",
	            "{\"id\":\"free\",\"owner\":\"alpha\",\"text\":\"mine\"}");
	list = run(module, "bar-slot-list", "alpha");
	g_assert_nonnull(strstr(list, "\"text\":\"one\""));
	g_assert_null(strstr(list, "mine"));
	/* The filter is by owner. */
	g_assert_null(strstr(list, "three"));
	g_assert_null(strstr(list, "four"));
	g_clear_pointer(&list, g_free);

	/* Removal by owner refuses another's; removal by name alone is the
	   user's and takes anything. */
	run_refused(module, "bar-slot-remove", "a1 beta");
	run_ok(module, "bar-slot-remove", "a1 alpha");
	run_ok(module, "bar-slot-remove", "free");
	run_refused(module, "bar-slot-remove", "free");
	run_refused(module, "bar-slot-remove", "");
	g_assert_cmpint(widget_count(module, "slot:a1"), ==, 0);
	g_assert_cmpint(widget_count(module, "slot:free"), ==, 0);

	/* An owner going away takes what is its own and nothing else. */
	run_ok(module, "bar-slot-set",
	       "{\"id\":\"a3\",\"owner\":\"alpha\",\"text\":\"five\"}");
	reply = run(module, "bar-slot-clear", "alpha");
	g_assert_cmpstr(reply, ==, "removed 2\n");
	g_assert_cmpint(widget_count(module, "slot:a2"), ==, 0);
	g_assert_cmpint(widget_count(module, "slot:a3"), ==, 0);
	g_assert_cmpint(widget_count(module, "slot:b1"), ==, 1);
	g_clear_pointer(&reply, g_free);

	/* Clearing an owner with nothing is not an error. */
	reply = run(module, "bar-slot-clear", "alpha");
	g_assert_cmpstr(reply, ==, "removed 0\n");
	g_clear_pointer(&reply, g_free);

	/* "Everything" is asked for by name, never by leaving the owner
	   out. */
	run_refused(module, "bar-slot-clear", "");
	run_refused(module, "bar-slot-clear", NULL);
	g_assert_cmpint(widget_count(module, "slot:b1"), ==, 1);
	reply = run(module, "bar-slot-clear", "--all");
	g_assert_cmpstr(reply, ==, "removed 1\n");
	list = run(module, "bar-slot-list", NULL);
	g_assert_cmpstr(list, ==, "[]\n");

	g_object_unref(module);
}

static void
test_order_among_a_regions_widgets(void)
{
	GowlModule *module;

	module = load_bar_module();
	if (module == NULL)
		return;

	configure(module, "widgets-right", "cpu memory");

	run_ok(module, "bar-slot-set", "{\"id\":\"late\",\"order\":20}");
	run_ok(module, "bar-slot-set", "{\"id\":\"early\",\"order\":10}");
	run_ok(module, "bar-slot-set", "{\"id\":\"first\",\"order\":-1}");
	run_ok(module, "bar-slot-set", "{\"id\":\"tie-a\",\"order\":15}");
	run_ok(module, "bar-slot-set", "{\"id\":\"tie-b\",\"order\":15}");

	/* Negative: before the region's own widgets.  Zero and up: after
	   them, by order, equal orders in the order they were made. */
	g_assert_cmpint(widget_index(module, "slot:first"), <,
	                widget_index(module, "cpu"));
	g_assert_cmpint(widget_index(module, "cpu"), <,
	                widget_index(module, "memory"));
	g_assert_cmpint(widget_index(module, "memory"), <,
	                widget_index(module, "slot:early"));
	g_assert_cmpint(widget_index(module, "slot:early"), <,
	                widget_index(module, "slot:tie-a"));
	g_assert_cmpint(widget_index(module, "slot:tie-a"), <,
	                widget_index(module, "slot:tie-b"));
	g_assert_cmpint(widget_index(module, "slot:tie-b"), <,
	                widget_index(module, "slot:late"));

	/* A new order moves it. */
	run_ok(module, "bar-slot-set", "{\"id\":\"late\",\"order\":-5}");
	g_assert_cmpint(widget_index(module, "slot:late"), <,
	                widget_index(module, "slot:first"));
	g_assert_cmpint(widget_count(module, "slot:late"), ==, 1);

	g_object_unref(module);
}

static void
test_a_slot_moves_between_regions_and_bars(void)
{
	GowlModule *module;
	g_autofree gchar *line = NULL;

	module = load_bar_module();
	if (module == NULL)
		return;

	run_ok(module, "bar-slot-set", "mover here");
	line = widget_line(module, "slot:mover", NULL);
	g_assert_nonnull(strstr(line, "right"));
	g_clear_pointer(&line, g_free);

	run_ok(module, "bar-slot-set", "{\"id\":\"mover\",\"region\":\"left\"}");
	line = widget_line(module, "slot:mover", NULL);
	g_assert_nonnull(strstr(line, "left"));
	/* Moving is not a second copy, and does not cost the text. */
	g_assert_cmpint(widget_count(module, "slot:mover"), ==, 1);
	g_assert_nonnull(strstr(line, "here"));
	g_clear_pointer(&line, g_free);

	g_assert_cmpint(widget_index(module, "slot:mover"), <,
	                bottom_heading_index(module));
	run_ok(module, "bar-slot-set", "{\"id\":\"mover\",\"bar\":\"bottom\"}");
	g_assert_cmpint(widget_count(module, "slot:mover"), ==, 1);
	g_assert_cmpint(widget_index(module, "slot:mover"), >,
	                bottom_heading_index(module));
	line = widget_line(module, "slot:mover", NULL);
	g_assert_nonnull(strstr(line, "here"));

	g_object_unref(module);
}

/*
 * The property the whole design leans on: a slot is not part of any
 * widget list, so the configuration that rebuilds a list -- every
 * reload, and every setting cmacs pushes -- does not take it.
 */
static void
test_a_slot_survives_configuration(void)
{
	GowlModule *module;
	g_autofree gchar *line = NULL;

	module = load_bar_module();
	if (module == NULL)
		return;

	run_ok(module, "bar-slot-set", "keep kept");
	run_ok(module, "bar-slot-set", "{\"id\":\"lead\",\"order\":-1}");

	/* Replace the region it is in, wholesale. */
	configure(module, "widgets-right", "cpu memory");
	g_assert_cmpint(widget_count(module, "slot:keep"), ==, 1);
	line = widget_line(module, "slot:keep", NULL);
	g_assert_nonnull(strstr(line, "kept"));
	g_clear_pointer(&line, g_free);

	/* And it is still on the right side of the widgets that arrived
	   after it: the list's own come first, a non-negative slot last. */
	g_assert_cmpint(widget_index(module, "slot:lead"), <,
	                widget_index(module, "cpu"));
	g_assert_cmpint(widget_index(module, "memory"), <,
	                widget_index(module, "slot:keep"));

	/* Again, with a different list: it is not luck of the first. */
	configure(module, "widgets-right", "battery");
	g_assert_cmpint(widget_count(module, "slot:keep"), ==, 1);
	g_assert_cmpint(widget_index(module, "battery"), <,
	                widget_index(module, "slot:keep"));

	/* The amendment key that removes by spec does not remove a slot
	   either: that is what `bar-slot-remove' is for. */
	configure(module, "widgets-right-remove", "slot:keep");
	g_assert_cmpint(widget_count(module, "slot:keep"), ==, 1);

	/* A setting pushed to some other widget -- the commonest
	   configuration there is under cmacs -- changes nothing. */
	configure(module, "battery.color", "green");
	g_assert_cmpint(widget_count(module, "slot:keep"), ==, 1);
	g_assert_cmpint(widget_count(module, "slot:lead"), ==, 1);

	run_ok(module, "bar-slot-remove", "keep");
	g_assert_cmpint(widget_count(module, "slot:keep"), ==, 0);
	configure(module, "widgets-right", "cpu");
	g_assert_cmpint(widget_count(module, "slot:keep"), ==, 0);

	g_object_unref(module);
}

/*
 * A `slot:ID' written into a widget list is a place the user chose.
 * A push to ID fills it there; the slot's own bar, region and order do
 * not apply, and removing the slot empties the place without taking it
 * out of the user's list.
 */
static void
test_a_listed_slot_is_filled_in_place(void)
{
	GowlModule *module;
	g_autofree gchar *line = NULL;
	g_autofree gchar *list = NULL;

	module = load_bar_module();
	if (module == NULL)
		return;

	configure(module, "widgets-left", "tags slot:pin title");
	g_assert_cmpint(widget_count(module, "slot:pin"), ==, 1);

	run_ok(module, "bar-slot-set",
	       "{\"id\":\"pin\",\"text\":\"filled\",\"region\":\"right\"}");
	g_assert_cmpint(widget_count(module, "slot:pin"), ==, 1);
	line = widget_line(module, "slot:pin", NULL);
	g_assert_nonnull(strstr(line, "filled"));
	g_assert_nonnull(strstr(line, "left"));
	g_clear_pointer(&line, g_free);
	g_assert_cmpint(widget_index(module, "tags"), <,
	                widget_index(module, "slot:pin"));
	g_assert_cmpint(widget_index(module, "slot:pin"), <,
	                widget_index(module, "title"));

	/* Removed: the place stays, empty. */
	run_ok(module, "bar-slot-remove", "pin");
	g_assert_cmpint(widget_count(module, "slot:pin"), ==, 1);
	line = widget_line(module, "slot:pin", NULL);
	g_assert_null(strstr(line, "filled"));
	g_clear_pointer(&line, g_free);
	list = run(module, "bar-slot-list", NULL);
	g_assert_cmpstr(list, ==, "[]\n");

	/* The other way round: the slot first, on its own; then a list
	   names it, and the one item moves to where the list says. */
	run_ok(module, "bar-slot-set", "roam wandering");
	line = widget_line(module, "slot:roam", NULL);
	g_assert_nonnull(strstr(line, "right"));
	g_clear_pointer(&line, g_free);

	configure(module, "widgets-left", "tags slot:roam title");
	g_assert_cmpint(widget_count(module, "slot:roam"), ==, 1);
	line = widget_line(module, "slot:roam", NULL);
	g_assert_nonnull(strstr(line, "left"));
	g_assert_nonnull(strstr(line, "wandering"));
	g_clear_pointer(&line, g_free);

	/* And when the list stops naming it, the slot is still a slot: it
	   goes back to the place it asked for itself. */
	configure(module, "widgets-left", "tags title");
	g_assert_cmpint(widget_count(module, "slot:roam"), ==, 1);
	line = widget_line(module, "slot:roam", NULL);
	g_assert_nonnull(strstr(line, "right"));
	g_assert_nonnull(strstr(line, "wandering"));

	g_object_unref(module);
}

/* An empty slot is not shown, the way a battery widget is not shown on
   a desktop: `bar-widgets' lists it with no label. */
static void
test_an_empty_slot_takes_no_room(void)
{
	GowlModule *module;
	g_autofree gchar *line = NULL;

	module = load_bar_module();
	if (module == NULL)
		return;

	run_ok(module, "bar-slot-set", "quiet something");
	run_ok(module, "bar-slot-set", "quiet");
	g_assert_cmpint(widget_count(module, "slot:quiet"), ==, 1);
	line = widget_line(module, "slot:quiet", NULL);
	g_assert_null(strstr(line, "something"));

	g_object_unref(module);
}

int
main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/bar-slots/created-and-updated",
	                test_a_slot_is_created_and_updated);
	g_test_add_func("/bar-slots/json-carries-every-field",
	                test_json_carries_every_field);
	g_test_add_func("/bar-slots/what-a-push-may-not-say",
	                test_what_a_push_may_not_say);
	g_test_add_func("/bar-slots/text-is-bounded", test_text_is_bounded);
	g_test_add_func("/bar-slots/capped", test_slots_are_capped);
	g_test_add_func("/bar-slots/owners", test_an_owner_keeps_its_slots);
	g_test_add_func("/bar-slots/order",
	                test_order_among_a_regions_widgets);
	g_test_add_func("/bar-slots/moves",
	                test_a_slot_moves_between_regions_and_bars);
	g_test_add_func("/bar-slots/survives-configuration",
	                test_a_slot_survives_configuration);
	g_test_add_func("/bar-slots/listed-slot-filled-in-place",
	                test_a_listed_slot_is_filled_in_place);
	g_test_add_func("/bar-slots/empty-takes-no-room",
	                test_an_empty_slot_takes_no_room);

	return g_test_run();
}
