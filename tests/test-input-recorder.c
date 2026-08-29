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
 * Tests for GowlInputRecorder: the consent gate, the bounded ring and
 * its dropped count, motion coalescing, the self-stop deadline, the
 * escape hatch, the secret-suppression policy, and the shape of the
 * JSON payload.  All run with no compositor and no wlroots -- the
 * recorder is fed through plain C calls, which is the point of keeping
 * the policy out of gowl-compositor.c.
 */

#include <glib-object.h>
#include <json-glib/json-glib.h>

#include "core/gowl-compositor.h"
#include "core/gowl-input-recorder.h"
#include "config/gowl-config.h"

/* ------------------------------------------------------------------ *
 * Helpers
 * ------------------------------------------------------------------ */

/* A recorder with consent already given, which is what most tests want
 * to exercise.  The consent gate itself is tested separately. */
static GowlInputRecorder *
consented(void)
{
	GowlInputRecorder *r;

	r = gowl_input_recorder_new();
	gowl_input_recorder_set_consent(r, TRUE);
	return r;
}

static void
note_key(GowlInputRecorder *r, guint32 keycode, const gchar *reason)
{
	GowlRecordedEvent ev;

	memset(&ev, 0, sizeof ev);
	ev.type    = GOWL_RECORDED_EVENT_KEY;
	ev.keycode = keycode;
	ev.keysym  = 0x0061; /* XKB_KEY_a */
	ev.state   = 1;
	gowl_input_recorder_note(r, &ev, reason);
}

static void
note_motion(GowlInputRecorder *r, gdouble dx, gdouble dy)
{
	GowlRecordedEvent ev;

	memset(&ev, 0, sizeof ev);
	ev.type = GOWL_RECORDED_EVENT_POINTER_MOTION;
	ev.dx   = dx;
	ev.dy   = dy;
	ev.x    = 100.0 + dx;
	ev.y    = 200.0 + dy;
	gowl_input_recorder_note(r, &ev, NULL);
}

/* Parses a payload and returns its root object.  The caller owns the
 * parser and must keep it alive while using the object. */
static JsonObject *
parse(const gchar *json, JsonParser **parser_out)
{
	JsonParser *parser;
	GError     *error = NULL;

	parser = json_parser_new();
	g_assert_true(json_parser_load_from_data(parser, json, -1, &error));
	g_assert_no_error(error);
	*parser_out = parser;

	return json_node_get_object(json_parser_get_root(parser));
}

static JsonArray *
events_of(JsonObject *root)
{
	g_assert_true(json_object_has_member(root, "events"));
	return json_object_get_array_member(root, "events");
}

/* ------------------------------------------------------------------ *
 * Consent
 * ------------------------------------------------------------------ */

/*
 * The whole point of the feature's permission story: a fresh recorder
 * refuses, and the refusal names the switch rather than being a bare
 * "denied" somebody has to go and look up.
 */
static void
test_consent_default_off(void)
{
	GowlInputRecorder *r;
	GError            *error = NULL;
	gchar             *token;

	r = gowl_input_recorder_new();
	g_assert_false(gowl_input_recorder_get_consent(r));

	token = gowl_input_recorder_start(r, 10, 16, &error);
	g_assert_null(token);
	g_assert_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED);
	g_assert_nonnull(g_strstr_len(error->message, -1, "input-recording"));
	g_clear_error(&error);

	g_object_unref(r);
}

static void
test_consent_allows_start(void)
{
	GowlInputRecorder *r;
	GError            *error = NULL;
	g_autofree gchar  *token = NULL;

	r = consented();
	token = gowl_input_recorder_start(r, 10, 16, &error);
	g_assert_no_error(error);
	g_assert_nonnull(token);
	g_assert_cmpuint(strlen(token), ==, 32);
	g_assert_true(gowl_input_recorder_is_active(r));

	g_object_unref(r);
}

/*
 * Withdrawing consent has to stop the recording that is running, not
 * merely refuse the next one.  A switch that only applies to the future
 * is not a switch anybody can use to make the thing stop now.
 */
static void
test_withdrawing_consent_stops_recording(void)
{
	GowlInputRecorder *r;
	g_autofree gchar  *token = NULL;

	r = consented();
	token = gowl_input_recorder_start(r, 10, 16, NULL);
	g_assert_true(gowl_input_recorder_is_active(r));

	gowl_input_recorder_set_consent(r, FALSE);
	g_assert_false(gowl_input_recorder_is_active(r));

	g_object_unref(r);
}

/* Two recordings at once would make "drain the token I hold" ambiguous;
 * the refusal names the running token so the caller can act on it. */
static void
test_second_start_refused(void)
{
	GowlInputRecorder *r;
	GError            *error = NULL;
	g_autofree gchar  *first = NULL;
	gchar             *second;

	r = consented();
	first = gowl_input_recorder_start(r, 10, 16, NULL);

	second = gowl_input_recorder_start(r, 10, 16, &error);
	g_assert_null(second);
	g_assert_error(error, G_IO_ERROR, G_IO_ERROR_BUSY);
	g_assert_nonnull(g_strstr_len(error->message, -1, first));
	g_clear_error(&error);

	g_object_unref(r);
}

/* ------------------------------------------------------------------ *
 * The ring
 * ------------------------------------------------------------------ */

static void
test_ring_records_in_order(void)
{
	GowlInputRecorder *r;
	g_autofree gchar  *token = NULL;
	g_autofree gchar  *json  = NULL;
	JsonParser        *parser = NULL;
	JsonObject        *root;
	JsonArray         *events;
	guint              i;

	r = consented();
	token = gowl_input_recorder_start(r, 60, 16, NULL);

	for (i = 0; i < 5; i++)
		note_key(r, 30 + i, NULL);

	json = gowl_input_recorder_drain(r, token, NULL);
	root = parse(json, &parser);
	events = events_of(root);

	g_assert_cmpuint(json_array_get_length(events), ==, 5);
	for (i = 0; i < 5; i++) {
		JsonObject *e = json_array_get_object_element(events, i);

		g_assert_cmpstr(json_object_get_string_member(e, "type"),
		                ==, "key");
		g_assert_cmpint(json_object_get_int_member(e, "keycode"),
		                ==, 30 + i);
	}
	g_assert_cmpint(json_object_get_int_member(root, "dropped"), ==, 0);

	g_object_unref(parser);
	g_object_unref(r);
}

/*
 * The limit needs a test that reaches it.  Twenty events into a
 * four-slot ring must leave the last four and report sixteen dropped --
 * silently keeping four and calling it the demonstration is how a
 * recorder teaches half a task.
 */
static void
test_ring_drops_oldest_and_reports(void)
{
	GowlInputRecorder *r;
	g_autofree gchar  *token = NULL;
	g_autofree gchar  *json  = NULL;
	JsonParser        *parser = NULL;
	JsonObject        *root;
	JsonArray         *events;
	JsonObject        *first;
	guint              i;

	r = consented();
	token = gowl_input_recorder_start(r, 60, 4, NULL);

	for (i = 0; i < 20; i++)
		note_key(r, 100 + i, NULL);

	json = gowl_input_recorder_drain(r, token, NULL);
	root = parse(json, &parser);
	events = events_of(root);

	g_assert_cmpuint(json_array_get_length(events), ==, 4);
	g_assert_cmpint(json_object_get_int_member(root, "dropped"), ==, 16);
	g_assert_cmpint(json_object_get_int_member(root, "dropped_total"),
	                ==, 16);

	/* The tail, not the head. */
	first = json_array_get_object_element(events, 0);
	g_assert_cmpint(json_object_get_int_member(first, "keycode"),
	                ==, 116);

	g_object_unref(parser);
	g_object_unref(r);
}

/* A drain empties the ring and resets the per-drain counters, so a
 * consumer summing them across drains gets the true totals while the
 * *_total members keep the whole-recording view. */
static void
test_drain_empties_and_resets_counters(void)
{
	GowlInputRecorder *r;
	g_autofree gchar  *token = NULL;
	g_autofree gchar  *first = NULL;
	g_autofree gchar  *second = NULL;
	JsonParser        *p1 = NULL, *p2 = NULL;
	JsonObject        *r1, *r2;
	guint              i;

	r = consented();
	token = gowl_input_recorder_start(r, 60, 4, NULL);

	for (i = 0; i < 6; i++)
		note_key(r, 200 + i, NULL);
	first = gowl_input_recorder_drain(r, token, NULL);
	r1 = parse(first, &p1);
	g_assert_cmpint(json_object_get_int_member(r1, "dropped"), ==, 2);

	second = gowl_input_recorder_drain(r, token, NULL);
	r2 = parse(second, &p2);
	g_assert_cmpuint(json_array_get_length(events_of(r2)), ==, 0);
	g_assert_cmpint(json_object_get_int_member(r2, "dropped"), ==, 0);
	g_assert_cmpint(json_object_get_int_member(r2, "dropped_total"),
	                ==, 2);

	g_object_unref(p1);
	g_object_unref(p2);
	g_object_unref(r);
}

/*
 * A drag is hundreds of motion events; without coalescing they fill the
 * ring and the clicks fall out of the far end.  A merge keeps the
 * accumulated delta and the later position, and is counted as a merge
 * rather than as a drop.
 */
static void
test_motion_coalesces(void)
{
	GowlInputRecorder *r;
	g_autofree gchar  *token = NULL;
	g_autofree gchar  *json  = NULL;
	JsonParser        *parser = NULL;
	JsonObject        *root;
	JsonArray         *events;
	JsonObject        *e;
	guint              i;

	r = consented();
	token = gowl_input_recorder_start(r, 60, 64, NULL);

	for (i = 0; i < 10; i++)
		note_motion(r, 1.0, 2.0);

	json = gowl_input_recorder_drain(r, token, NULL);
	root = parse(json, &parser);
	events = events_of(root);

	/* Ten motions delivered back to back land in one entry. */
	g_assert_cmpuint(json_array_get_length(events), ==, 1);
	g_assert_cmpint(json_object_get_int_member(root, "coalesced"), ==, 9);
	g_assert_cmpint(json_object_get_int_member(root, "dropped"), ==, 0);

	e = json_array_get_object_element(events, 0);
	g_assert_cmpstr(json_object_get_string_member(e, "type"), ==,
	                "pointer_motion");
	g_assert_cmpfloat(json_object_get_double_member(e, "dx"), ==, 10.0);
	g_assert_cmpfloat(json_object_get_double_member(e, "dy"), ==, 20.0);
	g_assert_cmpint(json_object_get_int_member(e, "merged"), ==, 9);

	g_object_unref(parser);
	g_object_unref(r);
}

/* A key between two motions has to break the run, or a coalescing bug
 * would swallow the keystroke's position in the sequence. */
static void
test_motion_coalescing_stops_at_a_key(void)
{
	GowlInputRecorder *r;
	g_autofree gchar  *token = NULL;
	g_autofree gchar  *json  = NULL;
	JsonParser        *parser = NULL;
	JsonArray         *events;

	r = consented();
	token = gowl_input_recorder_start(r, 60, 64, NULL);

	note_motion(r, 1.0, 0.0);
	note_motion(r, 1.0, 0.0);
	note_key(r, 30, NULL);
	note_motion(r, 1.0, 0.0);

	json = gowl_input_recorder_drain(r, token, NULL);
	events = events_of(parse(json, &parser));

	g_assert_cmpuint(json_array_get_length(events), ==, 3);
	g_assert_cmpstr(json_object_get_string_member(
		json_array_get_object_element(events, 1), "type"), ==, "key");

	g_object_unref(parser);
	g_object_unref(r);
}

/* ------------------------------------------------------------------ *
 * Suppression
 * ------------------------------------------------------------------ */

static void
test_suppress_reason_lock_screen(void)
{
	GowlInputRecorder *r;

	r = gowl_input_recorder_new();
	g_assert_nonnull(gowl_input_recorder_suppress_reason(
		r, TRUE, "gst", "a terminal"));
	g_assert_null(gowl_input_recorder_suppress_reason(
		r, FALSE, "gst", "a terminal"));

	g_object_unref(r);
}

static void
test_suppress_reason_default_deny_list(void)
{
	GowlInputRecorder *r;

	r = gowl_input_recorder_new();

	/* app-id */
	g_assert_nonnull(gowl_input_recorder_suppress_reason(
		r, FALSE, "pinentry-gnome3", NULL));
	g_assert_nonnull(gowl_input_recorder_suppress_reason(
		r, FALSE, "org.keepassxc.KeePassXC", NULL));

	/* title, and case-insensitively */
	g_assert_nonnull(gowl_input_recorder_suppress_reason(
		r, FALSE, "librewolf", "Sign in - Forgejo"));
	g_assert_nonnull(gowl_input_recorder_suppress_reason(
		r, FALSE, "librewolf", "Enter Password"));

	/* an ordinary window is not suppressed */
	g_assert_null(gowl_input_recorder_suppress_reason(
		r, FALSE, "librewolf", "gowl: input recording"));

	g_object_unref(r);
}

/*
 * Extra patterns are *added*.  If a config value replaced the built-in
 * list, adding one pattern for a personal password manager would
 * silently drop the protection for pinentry and polkit.
 */
static void
test_added_deny_patterns_do_not_replace(void)
{
	GowlInputRecorder  *r;
	const gchar *const  extra[] = { "*my-vault*", NULL };

	r = gowl_input_recorder_new();
	gowl_input_recorder_add_deny_patterns(r, extra);

	g_assert_nonnull(gowl_input_recorder_suppress_reason(
		r, FALSE, "my-vault", NULL));
	g_assert_nonnull(gowl_input_recorder_suppress_reason(
		r, FALSE, "pinentry-gnome3", NULL));

	g_object_unref(r);
}

/*
 * A suppressed event must not be stored, and must not be silent either:
 * a gap the trace does not explain is indistinguishable from a gap in
 * the demonstration.
 */
static void
test_suppressed_events_are_withheld_and_counted(void)
{
	GowlInputRecorder *r;
	g_autofree gchar  *token = NULL;
	g_autofree gchar  *json  = NULL;
	JsonParser        *parser = NULL;
	JsonObject        *root;
	JsonArray         *reasons;

	r = consented();
	token = gowl_input_recorder_start(r, 60, 64, NULL);

	note_key(r, 30, NULL);
	note_key(r, 31, "the session is locked");
	note_key(r, 32, "the session is locked");
	note_key(r, 33, NULL);

	json = gowl_input_recorder_drain(r, token, NULL);
	root = parse(json, &parser);

	g_assert_cmpuint(json_array_get_length(events_of(root)), ==, 2);
	g_assert_cmpint(json_object_get_int_member(root, "suppressed"), ==, 2);
	g_assert_cmpint(json_object_get_int_member(root, "suppressed_total"),
	                ==, 2);

	reasons = json_object_get_array_member(root, "suppress_reasons");
	g_assert_cmpuint(json_array_get_length(reasons), ==, 1);
	g_assert_cmpstr(json_array_get_string_element(reasons, 0), ==,
	                "the session is locked");

	g_object_unref(parser);
	g_object_unref(r);
}

/* ------------------------------------------------------------------ *
 * Deadline, stop, escape hatch
 * ------------------------------------------------------------------ */

/*
 * The deadline is the answer to "a recording somebody forgot is a
 * recording of whatever they did next", so it has to hold with no
 * timer, no further input, and nobody polling: every entry point
 * re-checks it.
 */
static void
test_deadline_stops_recording(void)
{
	GowlInputRecorder *r;
	g_autofree gchar  *token = NULL;
	g_autofree gchar  *json  = NULL;
	JsonParser        *parser = NULL;
	JsonObject        *root;

	r = consented();
	token = gowl_input_recorder_start(r, 1, 64, NULL);
	note_key(r, 30, NULL);
	g_assert_true(gowl_input_recorder_is_active(r));

	g_usleep(1100 * 1000);

	g_assert_false(gowl_input_recorder_is_active(r));

	/* Input after the deadline is not recorded ... */
	note_key(r, 31, NULL);

	/* ... but what was captured before it is still drainable: the
	 * deadline stops the capture, not the reader. */
	json = gowl_input_recorder_drain(r, token, NULL);
	root = parse(json, &parser);
	g_assert_cmpuint(json_array_get_length(events_of(root)), ==, 1);
	g_assert_false(json_object_get_boolean_member(root, "active"));
	g_assert_cmpstr(json_object_get_string_member(root, "stop_reason"),
	                ==, "the maximum recording time elapsed");

	g_object_unref(parser);
	g_object_unref(r);
}

/* The signal is how the compositor knows to raise and lower the
 * indicator; it has to fire for a stop nobody asked for as well. */
static void
test_changed_signal_fires_for_start_and_deadline(void)
{
	GowlInputRecorder *r;
	g_autofree gchar  *token = NULL;
	gint               starts = 0;
	gint               stops  = 0;

	r = consented();
	g_signal_connect_swapped(r, "changed",
	                         G_CALLBACK(g_atomic_int_inc), &starts);
	token = gowl_input_recorder_start(r, 1, 8, NULL);
	g_assert_cmpint(starts, ==, 1);

	g_signal_connect_swapped(r, "changed",
	                         G_CALLBACK(g_atomic_int_inc), &stops);
	g_usleep(1100 * 1000);
	(void)gowl_input_recorder_is_active(r);
	g_assert_cmpint(stops, ==, 1);
	g_assert_cmpint(starts, ==, 2);

	g_object_unref(r);
}

/*
 * Super+Shift+Escape has to work for somebody who never had the token.
 * "Ask the agent to stop" is not a way out of being recorded.
 */
static void
test_force_stop_needs_no_token(void)
{
	GowlInputRecorder *r;
	g_autofree gchar  *token = NULL;
	g_autofree gchar  *json  = NULL;
	JsonParser        *parser = NULL;

	r = consented();
	token = gowl_input_recorder_start(r, 600, 64, NULL);
	note_key(r, 30, NULL);

	gowl_input_recorder_force_stop(r, "stopped from the keyboard");
	g_assert_false(gowl_input_recorder_is_active(r));
	note_key(r, 31, NULL);

	json = gowl_input_recorder_drain(r, token, NULL);
	g_assert_cmpuint(json_array_get_length(events_of(parse(json, &parser))),
	                 ==, 1);

	g_object_unref(parser);
	g_object_unref(r);
}

static void
test_stop_returns_tail_once(void)
{
	GowlInputRecorder *r;
	g_autofree gchar  *token  = NULL;
	g_autofree gchar  *first  = NULL;
	g_autofree gchar  *second = NULL;
	JsonParser        *p1 = NULL, *p2 = NULL;

	r = consented();
	token = gowl_input_recorder_start(r, 60, 64, NULL);
	note_key(r, 30, NULL);
	note_key(r, 31, NULL);

	first = gowl_input_recorder_stop(r, token, NULL);
	g_assert_cmpuint(json_array_get_length(events_of(parse(first, &p1))),
	                 ==, 2);

	second = gowl_input_recorder_stop(r, token, NULL);
	g_assert_cmpuint(json_array_get_length(events_of(parse(second, &p2))),
	                 ==, 0);

	g_object_unref(p1);
	g_object_unref(p2);
	g_object_unref(r);
}

static void
test_unknown_token_refused(void)
{
	GowlInputRecorder *r;
	GError            *error = NULL;
	g_autofree gchar  *token = NULL;

	r = consented();
	token = gowl_input_recorder_start(r, 60, 8, NULL);

	g_assert_null(gowl_input_recorder_drain(r, "not-a-token", &error));
	g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
	g_clear_error(&error);

	g_assert_null(gowl_input_recorder_stop(r, "not-a-token", &error));
	g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
	g_clear_error(&error);

	g_object_unref(r);
}

/* ------------------------------------------------------------------ *
 * Bounds and payload shape
 * ------------------------------------------------------------------ */

static void
test_limits_are_clamped(void)
{
	GowlInputRecorder *r;
	g_autofree gchar  *token = NULL;
	g_autofree gchar  *json  = NULL;
	JsonParser        *parser = NULL;
	JsonObject        *root;

	r = consented();
	token = gowl_input_recorder_start(r, 999999, 99999999, NULL);
	json = gowl_input_recorder_status(r);
	root = parse(json, &parser);

	g_assert_cmpint(json_object_get_int_member(root, "max_seconds"),
	                ==, GOWL_INPUT_RECORDER_MAX_SECONDS_CEILING);
	g_assert_cmpint(json_object_get_int_member(root, "max_events"),
	                ==, GOWL_INPUT_RECORDER_MAX_EVENTS_CEILING);

	g_object_unref(parser);
	g_object_unref(r);
}

static void
test_zero_limits_take_defaults(void)
{
	GowlInputRecorder *r;
	g_autofree gchar  *token = NULL;
	g_autofree gchar  *json  = NULL;
	JsonParser        *parser = NULL;
	JsonObject        *root;

	r = consented();
	token = gowl_input_recorder_start(r, 0, 0, NULL);
	json = gowl_input_recorder_status(r);
	root = parse(json, &parser);

	g_assert_cmpint(json_object_get_int_member(root, "max_seconds"),
	                ==, GOWL_INPUT_RECORDER_DEFAULT_MAX_SECONDS);
	g_assert_cmpint(json_object_get_int_member(root, "max_events"),
	                ==, GOWL_INPUT_RECORDER_DEFAULT_MAX_EVENTS);

	g_object_unref(parser);
	g_object_unref(r);
}

/*
 * The two-clocks rule, asserted rather than commented.  A recorded
 * event must carry wall time and an offset from the start of *this*
 * recording, and must NOT carry anything shaped like a compositor event
 * timestamp -- gowl_compositor_inject_* stamps synthetic events from
 * the same monotonic-milliseconds clock the real ones arrive on, so a
 * `time_msec` in the payload is an invitation to replay a captured time
 * as a synthetic one.
 */
static void
test_payload_carries_no_compositor_timestamp(void)
{
	GowlInputRecorder *r;
	g_autofree gchar  *token = NULL;
	g_autofree gchar  *json  = NULL;
	JsonParser        *parser = NULL;
	JsonObject        *e;

	r = consented();
	token = gowl_input_recorder_start(r, 60, 8, NULL);
	note_key(r, 30, NULL);
	json = gowl_input_recorder_drain(r, token, NULL);

	g_assert_null(g_strstr_len(json, -1, "time_msec"));

	e = json_array_get_object_element(
		events_of(parse(json, &parser)), 0);
	g_assert_true(json_object_has_member(e, "wall_us"));
	g_assert_true(json_object_has_member(e, "offset_ms"));
	g_assert_cmpint(json_object_get_int_member(e, "wall_us"), >,
	                G_GINT64_CONSTANT(1700000000000000));

	g_object_unref(parser);
	g_object_unref(r);
}

/* Wall times must not go backwards across a recording. */
static void
test_wall_times_are_non_decreasing(void)
{
	GowlInputRecorder *r;
	g_autofree gchar  *token = NULL;
	g_autofree gchar  *json  = NULL;
	JsonParser        *parser = NULL;
	JsonArray         *events;
	gint64             prev = 0;
	guint              i;

	r = consented();
	token = gowl_input_recorder_start(r, 60, 64, NULL);
	for (i = 0; i < 8; i++)
		note_key(r, 30 + i, NULL);

	json = gowl_input_recorder_drain(r, token, NULL);
	events = events_of(parse(json, &parser));

	for (i = 0; i < json_array_get_length(events); i++) {
		JsonObject *e = json_array_get_object_element(events, i);
		gint64 now = json_object_get_int_member(e, "wall_us");

		g_assert_cmpint(now, >=, prev);
		prev = now;
	}

	g_object_unref(parser);
	g_object_unref(r);
}

/* Every payload states the guard's reach, because a trace is reviewed
 * somewhere other than the documentation. */
static void
test_payload_states_suppression_limits(void)
{
	GowlInputRecorder *r;
	g_autofree gchar  *json = NULL;
	JsonParser        *parser = NULL;
	const gchar       *note;

	r = gowl_input_recorder_new();
	json = gowl_input_recorder_status(r);
	note = json_object_get_string_member(parse(json, &parser),
	                                     "secret_suppression");

	g_assert_nonnull(note);
	g_assert_nonnull(g_strstr_len(note, -1, "password field"));

	g_object_unref(parser);
	g_object_unref(r);
}

static void
test_note_before_start_is_ignored(void)
{
	GowlInputRecorder *r;
	g_autofree gchar  *json = NULL;
	JsonParser        *parser = NULL;
	JsonObject        *root;

	r = consented();
	note_key(r, 30, NULL);

	json = gowl_input_recorder_status(r);
	root = parse(json, &parser);
	g_assert_false(json_object_get_boolean_member(root, "active"));
	g_assert_cmpint(json_object_get_int_member(root, "buffered"), ==, 0);

	g_object_unref(parser);
	g_object_unref(r);
}

/* ------------------------------------------------------------------ *
 * Compositor wiring
 *
 * The recorder is only useful if the config key reaches it, and only
 * safe if it keeps reaching it.  These run against a real
 * GowlCompositor that was never started -- no backend, no display, no
 * scene -- which is enough to exercise the plumbing that a unit test of
 * the recorder alone cannot see.
 * ------------------------------------------------------------------ */

static void
test_compositor_owns_a_recorder(void)
{
	GowlCompositor    *c;
	GowlInputRecorder *rec;

	c = gowl_compositor_new();
	rec = gowl_compositor_get_input_recorder(c);

	/* Always present, so "not recording" and "no recorder" cannot be
	 * confused by a caller. */
	g_assert_nonnull(rec);
	g_assert_false(gowl_input_recorder_get_consent(rec));

	g_object_unref(c);
}

/*
 * The consent key has to be *followed*, not sampled once at startup, or
 * turning it off would need the session restarted.  Setting the
 * property after set_config() is the reload path in miniature.
 */
static void
test_config_consent_reaches_recorder(void)
{
	GowlCompositor    *c;
	GowlConfig        *cfg;
	GowlInputRecorder *rec;

	c   = gowl_compositor_new();
	cfg = gowl_config_new();
	gowl_compositor_set_config(c, cfg);
	rec = gowl_compositor_get_input_recorder(c);

	g_assert_false(gowl_input_recorder_get_consent(rec));

	g_object_set(cfg, "input-recording", TRUE, NULL);
	g_assert_true(gowl_input_recorder_get_consent(rec));

	g_object_set(cfg, "input-recording", FALSE, NULL);
	g_assert_false(gowl_input_recorder_get_consent(rec));

	g_object_unref(c);
	g_object_unref(cfg);
}

/* Extra deny patterns arrive through the config too, split on commas
 * and stripped, and adding them keeps the built-in list. */
static void
test_config_deny_apps_reach_recorder(void)
{
	GowlCompositor    *c;
	GowlConfig        *cfg;
	GowlInputRecorder *rec;

	c   = gowl_compositor_new();
	cfg = gowl_config_new();
	gowl_compositor_set_config(c, cfg);
	rec = gowl_compositor_get_input_recorder(c);

	g_object_set(cfg, "input-recording-deny-apps",
	             " *my-vault* , *corp-sso* ", NULL);

	g_assert_nonnull(gowl_input_recorder_suppress_reason(
		rec, FALSE, "my-vault-desktop", NULL));
	g_assert_nonnull(gowl_input_recorder_suppress_reason(
		rec, FALSE, "corp-sso", NULL));
	g_assert_nonnull(gowl_input_recorder_suppress_reason(
		rec, FALSE, "pinentry-gnome3", NULL));
	g_assert_null(gowl_input_recorder_suppress_reason(
		rec, FALSE, "gst", "a terminal"));

	/* Applying the config twice must not accumulate: the reset to the
	 * built-in list happens before the additions. */
	gowl_compositor_apply_input_recording_config(c);
	g_assert_nonnull(gowl_input_recorder_suppress_reason(
		rec, FALSE, "my-vault-desktop", NULL));

	g_object_unref(c);
	g_object_unref(cfg);
}

/* Clearing the key must remove the pattern rather than leave the last
 * value in force. */
static void
test_config_deny_apps_cleared(void)
{
	GowlCompositor    *c;
	GowlConfig        *cfg;
	GowlInputRecorder *rec;

	c   = gowl_compositor_new();
	cfg = gowl_config_new();
	gowl_compositor_set_config(c, cfg);
	rec = gowl_compositor_get_input_recorder(c);

	g_object_set(cfg, "input-recording-deny-apps", "*my-vault*", NULL);
	g_assert_nonnull(gowl_input_recorder_suppress_reason(
		rec, FALSE, "my-vault", NULL));

	g_object_set(cfg, "input-recording-deny-apps", "", NULL);
	g_assert_null(gowl_input_recorder_suppress_reason(
		rec, FALSE, "my-vault", NULL));
	g_assert_nonnull(gowl_input_recorder_suppress_reason(
		rec, FALSE, "pinentry-gnome3", NULL));

	g_object_unref(c);
	g_object_unref(cfg);
}

static void
test_recorder_gtype_is_object(void)
{
	g_assert_true(g_type_is_a(GOWL_TYPE_INPUT_RECORDER, G_TYPE_OBJECT));
}

int
main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/inputrecorder/consent/default-off",
	                test_consent_default_off);
	g_test_add_func("/inputrecorder/consent/allows-start",
	                test_consent_allows_start);
	g_test_add_func("/inputrecorder/consent/withdraw-stops",
	                test_withdrawing_consent_stops_recording);
	g_test_add_func("/inputrecorder/consent/second-start-refused",
	                test_second_start_refused);

	g_test_add_func("/inputrecorder/ring/order",
	                test_ring_records_in_order);
	g_test_add_func("/inputrecorder/ring/drops-oldest",
	                test_ring_drops_oldest_and_reports);
	g_test_add_func("/inputrecorder/ring/drain-resets",
	                test_drain_empties_and_resets_counters);
	g_test_add_func("/inputrecorder/ring/motion-coalesces",
	                test_motion_coalesces);
	g_test_add_func("/inputrecorder/ring/motion-run-broken-by-key",
	                test_motion_coalescing_stops_at_a_key);

	g_test_add_func("/inputrecorder/suppress/lock-screen",
	                test_suppress_reason_lock_screen);
	g_test_add_func("/inputrecorder/suppress/deny-list",
	                test_suppress_reason_default_deny_list);
	g_test_add_func("/inputrecorder/suppress/added-patterns-add",
	                test_added_deny_patterns_do_not_replace);
	g_test_add_func("/inputrecorder/suppress/withheld-and-counted",
	                test_suppressed_events_are_withheld_and_counted);

	g_test_add_func("/inputrecorder/deadline/stops",
	                test_deadline_stops_recording);
	g_test_add_func("/inputrecorder/deadline/signal",
	                test_changed_signal_fires_for_start_and_deadline);
	g_test_add_func("/inputrecorder/stop/force-no-token",
	                test_force_stop_needs_no_token);
	g_test_add_func("/inputrecorder/stop/tail-once",
	                test_stop_returns_tail_once);
	g_test_add_func("/inputrecorder/stop/unknown-token",
	                test_unknown_token_refused);

	g_test_add_func("/inputrecorder/limits/clamped",
	                test_limits_are_clamped);
	g_test_add_func("/inputrecorder/limits/defaults",
	                test_zero_limits_take_defaults);

	g_test_add_func("/inputrecorder/payload/no-compositor-timestamp",
	                test_payload_carries_no_compositor_timestamp);
	g_test_add_func("/inputrecorder/payload/wall-times-monotonic",
	                test_wall_times_are_non_decreasing);
	g_test_add_func("/inputrecorder/payload/states-limits",
	                test_payload_states_suppression_limits);
	g_test_add_func("/inputrecorder/note/before-start",
	                test_note_before_start_is_ignored);
	g_test_add_func("/inputrecorder/compositor/owns-recorder",
	                test_compositor_owns_a_recorder);
	g_test_add_func("/inputrecorder/compositor/consent-follows-config",
	                test_config_consent_reaches_recorder);
	g_test_add_func("/inputrecorder/compositor/deny-apps-from-config",
	                test_config_deny_apps_reach_recorder);
	g_test_add_func("/inputrecorder/compositor/deny-apps-cleared",
	                test_config_deny_apps_cleared);

	g_test_add_func("/inputrecorder/gtype",
	                test_recorder_gtype_is_object);

	return g_test_run();
}
