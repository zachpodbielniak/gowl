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
 * GowlInputRecorder -- a bounded observer of real input, for turning a
 * human demonstration into a reusable procedure.
 *
 * It is the mirror image of the injection tools in
 * modules/mcp/gowl-mcp-tools-input.c, and it is deliberately *not*
 * gated by the same switch: injecting a click is a different permission
 * from watching somebody type, and folding the two together would make
 * "may click for me" quietly mean "may keylog me".  Recording is off
 * until the `input-recording` config key says otherwise, and the
 * compositor paints an unmissable frame around the screen for as long
 * as a recording runs.
 *
 * Like GowlInputCapture it links no wlroots and no Wayland: the
 * compositor feeds it finished values through gowl_input_recorder_note()
 * and tells it the three facts the suppression policy needs.  That keeps
 * the ring, the deadline and the policy unit-testable with no
 * compositor, which for a keylogger guard is the difference between a
 * tested rule and a hopeful one.
 *
 * What it can and cannot protect against is written down in
 * docs/input-recording.org and repeated in every status payload: gowl
 * has no way to see that a client's focused widget is a password entry,
 * because Wayland gives a compositor no window-internal knowledge at
 * all.  The lock screen it *can* see; a credential prompt it can only
 * recognise by app-id or title.
 */

#include <gio/gio.h>
#include <json-glib/json-glib.h>
#include <xkbcommon/xkbcommon.h>
#include <stdio.h>

#include "gowl-input-recorder.h"

/*
 * Default deny patterns: glob patterns matched case-insensitively
 * against the focused client's app-id and title.  This is a mitigation,
 * not a guarantee -- see the file comment.  It is overridable through
 * the `input-recording-deny-apps` config key precisely because a list
 * of everybody's password prompts cannot be maintained here.
 */
static const gchar *const default_deny_patterns[] = {
	"*pinentry*",
	"*polkit*",
	"*askpass*",
	"*gcr-prompter*",
	"*org.gnome.keyring*",
	"*seahorse*",
	"*keepass*",
	"*bitwarden*",
	"*1password*",
	"*vaultwarden*",
	"*proton*pass*",
	"*swaylock*",
	"*gtklock*",
	"*password*",
	"*passphrase*",
	"*unlock*",
	"*sign in*",
	"*log in*",
	"*login*",
	NULL
};

struct _GowlInputRecorder {
	GObject parent_instance;

	/* consent, entirely separate from whatever gates injection */
	gboolean            consent;

	/* deny patterns, owned; NULL-terminated */
	gchar             **deny_patterns;
	GPtrArray          *deny_specs;    /* GPatternSpec*, parallel */

	/* active recording state */
	gboolean            active;
	gchar              *token;

	GowlRecordedEvent  *ring;          /* capacity entries, or NULL */
	guint               capacity;
	guint               head;          /* index of the oldest entry */
	guint               count;         /* entries currently held */

	gint64              started_wall_us;
	gint64              started_mono_us;
	gint64              deadline_us;   /* monotonic; 0 = no deadline */
	guint               max_seconds;

	/* counters.  Each has a "since the last drain" and a whole-recording
	 * form, because a consumer that drains repeatedly needs the first and
	 * a person reviewing the trace needs the second. */
	guint64             dropped;
	guint64             dropped_total;
	guint64             suppressed;
	guint64             suppressed_total;
	guint64             coalesced;
	guint64             coalesced_total;

	/* every distinct suppression reason seen during this recording */
	GHashTable         *suppress_reasons;

	/* why the last recording ended, a static literal or NULL */
	const gchar        *stop_reason;
};

enum {
	SIGNAL_CHANGED,
	SIGNAL_LAST
};

static guint signals[SIGNAL_LAST] = { 0 };

G_DEFINE_TYPE(GowlInputRecorder, gowl_input_recorder, G_TYPE_OBJECT)

/* ---------------------------------------------------------------
 * Helpers
 * --------------------------------------------------------------- */

/*
 * new_token:
 *
 * 16 bytes of /dev/urandom, hex-encoded.  The token is a handle rather
 * than a capability -- anybody who can call drain_recording could have
 * called start_recording -- but it costs nothing to make it
 * unguessable, and g_random_* is a Mersenne Twister.  If /dev/urandom
 * cannot be read the recorder refuses to start rather than falling back
 * to a predictable token: a recorder that cannot be started is a
 * feature that is missing, and one with a guessable handle is a feature
 * that lies.
 */
static gchar *
new_token(void)
{
	FILE   *f;
	guchar  buf[16];
	gsize   got;
	GString *out;
	gsize   i;

	f = fopen("/dev/urandom", "rb");
	if (f == NULL)
		return NULL;

	got = fread(buf, 1, sizeof buf, f);
	fclose(f);

	if (got != sizeof buf)
		return NULL;

	out = g_string_sized_new(sizeof buf * 2 + 1);
	for (i = 0; i < sizeof buf; i++)
		g_string_append_printf(out, "%02x", buf[i]);

	return g_string_free(out, FALSE);
}

/*
 * Typed wrappers for g_clear_pointer.  Casting g_ptr_array_unref or
 * g_hash_table_unref to GDestroyNotify and calling through the cast is
 * a call through an incompatible type, which gcc warns about and which
 * is undefined besides.
 */
static void
free_pattern_spec(gpointer spec)
{
	g_pattern_spec_free((GPatternSpec *)spec);
}

static void
unref_ptr_array(GPtrArray *array)
{
	g_ptr_array_unref(array);
}

static void
unref_hash_table(GHashTable *table)
{
	g_hash_table_unref(table);
}

/*
 * rebuild_deny_specs:
 *
 * Compiles self->deny_patterns into GPatternSpecs once, so the match on
 * every key event is a compiled-glob test and not a parse.
 */
static void
rebuild_deny_specs(GowlInputRecorder *self)
{
	gsize i;

	g_clear_pointer(&self->deny_specs, unref_ptr_array);
	self->deny_specs = g_ptr_array_new_with_free_func(free_pattern_spec);

	if (self->deny_patterns == NULL)
		return;

	for (i = 0; self->deny_patterns[i] != NULL; i++) {
		g_autofree gchar *lower = NULL;

		lower = g_ascii_strdown(self->deny_patterns[i], -1);
		g_ptr_array_add(self->deny_specs, g_pattern_spec_new(lower));
	}
}

/*
 * matches_deny:
 *
 * Case-insensitive glob match of @text against the compiled deny list.
 */
static gboolean
matches_deny(GowlInputRecorder *self, const gchar *text)
{
	g_autofree gchar *lower = NULL;
	guint i;

	if (text == NULL || *text == '\0' || self->deny_specs == NULL)
		return FALSE;

	lower = g_ascii_strdown(text, -1);
	for (i = 0; i < self->deny_specs->len; i++) {
		if (g_pattern_spec_match_string(
			    (GPatternSpec *)self->deny_specs->pdata[i], lower))
			return TRUE;
	}

	return FALSE;
}

/*
 * reset_recording:
 *
 * Frees the ring and clears every per-recording counter.  Called from
 * stop and from finalize; never leaves a half-cleared state that a
 * later note() could append to.
 */
static void
reset_recording(GowlInputRecorder *self)
{
	g_clear_pointer(&self->ring, g_free);
	self->capacity         = 0;
	self->head             = 0;
	self->count            = 0;
	self->started_wall_us  = 0;
	self->started_mono_us  = 0;
	self->deadline_us      = 0;
	self->max_seconds      = 0;
	self->dropped          = 0;
	self->dropped_total    = 0;
	self->suppressed       = 0;
	self->suppressed_total = 0;
	self->coalesced        = 0;
	self->coalesced_total  = 0;
	g_hash_table_remove_all(self->suppress_reasons);
}

/* ---------------------------------------------------------------
 * GObject
 * --------------------------------------------------------------- */

static void
gowl_input_recorder_finalize(GObject *object)
{
	GowlInputRecorder *self = GOWL_INPUT_RECORDER(object);

	reset_recording(self);
	g_clear_pointer(&self->token, g_free);
	g_clear_pointer(&self->deny_patterns, g_strfreev);
	g_clear_pointer(&self->deny_specs, unref_ptr_array);
	g_clear_pointer(&self->suppress_reasons, unref_hash_table);

	G_OBJECT_CLASS(gowl_input_recorder_parent_class)->finalize(object);
}

static void
gowl_input_recorder_class_init(GowlInputRecorderClass *klass)
{
	G_OBJECT_CLASS(klass)->finalize = gowl_input_recorder_finalize;

	/**
	 * GowlInputRecorder::changed:
	 * @self: the #GowlInputRecorder
	 * @active: %TRUE if a recording is now running
	 * @token: (nullable): the token of the running recording, or %NULL
	 *
	 * Emitted whenever a recording starts or stops, however it stopped
	 * -- an explicit stop, the deadline expiring, or the escape hatch.
	 * This is the "recording state changed" notification: the compositor
	 * connects to it to raise and lower the on-screen indicator and to
	 * arm the self-stop timer, and an embedder or the MCP module can use
	 * it to tell a consumer without polling.
	 */
	signals[SIGNAL_CHANGED] = g_signal_new(
		"changed",
		G_TYPE_FROM_CLASS(klass),
		G_SIGNAL_RUN_LAST,
		0, NULL, NULL, NULL,
		G_TYPE_NONE, 2, G_TYPE_BOOLEAN, G_TYPE_STRING);
}

static void
gowl_input_recorder_init(GowlInputRecorder *self)
{
	self->consent          = FALSE;
	self->active           = FALSE;
	self->token            = NULL;
	self->ring             = NULL;
	self->stop_reason      = NULL;
	self->suppress_reasons = g_hash_table_new_full(
		g_str_hash, g_str_equal, g_free, NULL);
	self->deny_patterns    = g_strdupv((gchar **)default_deny_patterns);
	self->deny_specs       = NULL;
	rebuild_deny_specs(self);
}

/**
 * gowl_input_recorder_new:
 *
 * Creates a new #GowlInputRecorder.  Consent is off and no recording is
 * running.
 *
 * Returns: (transfer full): a new #GowlInputRecorder
 */
GowlInputRecorder *
gowl_input_recorder_new(void)
{
	return (GowlInputRecorder *)g_object_new(GOWL_TYPE_INPUT_RECORDER,
	                                          NULL);
}

/* ---------------------------------------------------------------
 * Consent
 * --------------------------------------------------------------- */

/**
 * gowl_input_recorder_get_consent:
 * @self: a #GowlInputRecorder
 *
 * Returns whether input recording has been consented to.
 *
 * Returns: %TRUE if recording may be started
 */
gboolean
gowl_input_recorder_get_consent(GowlInputRecorder *self)
{
	g_return_val_if_fail(GOWL_IS_INPUT_RECORDER(self), FALSE);

	return self->consent;
}

/**
 * gowl_input_recorder_set_consent:
 * @self: a #GowlInputRecorder
 * @consent: %TRUE to allow recordings to be started
 *
 * Sets the consent flag.  Withdrawing consent stops any recording that
 * is running: a switch that only applies to the next recording is not a
 * switch anybody can use to make the current one stop.
 */
void
gowl_input_recorder_set_consent(GowlInputRecorder *self, gboolean consent)
{
	g_return_if_fail(GOWL_IS_INPUT_RECORDER(self));

	consent = !!consent;
	if (self->consent == consent)
		return;

	self->consent = consent;
	if (!consent && self->active)
		gowl_input_recorder_force_stop(self, "consent withdrawn");
}

/* ---------------------------------------------------------------
 * Suppression policy
 * --------------------------------------------------------------- */

/**
 * gowl_input_recorder_set_deny_patterns:
 * @self: a #GowlInputRecorder
 * @patterns: (array zero-terminated=1) (nullable): glob patterns
 *
 * Replaces the deny list.  Patterns are globs, matched
 * case-insensitively against the focused client's app-id and title.
 * Passing %NULL restores the built-in list; passing an empty array
 * disables app matching entirely, which leaves only the lock screen.
 */
void
gowl_input_recorder_set_deny_patterns(GowlInputRecorder  *self,
                                      const gchar *const *patterns)
{
	g_return_if_fail(GOWL_IS_INPUT_RECORDER(self));

	g_clear_pointer(&self->deny_patterns, g_strfreev);
	self->deny_patterns = g_strdupv(
		patterns != NULL ? (gchar **)patterns
		                 : (gchar **)default_deny_patterns);
	rebuild_deny_specs(self);
}

/**
 * gowl_input_recorder_add_deny_patterns:
 * @self: a #GowlInputRecorder
 * @patterns: (array zero-terminated=1) (nullable): glob patterns to add
 *
 * Appends to the deny list, keeping what is already there.  This is what
 * the `input-recording-deny-apps` config key uses: a config value that
 * *replaced* the built-in list would let somebody add one pattern for
 * their own password manager and silently lose the protection for
 * pinentry and polkit.  Empty and duplicate patterns are ignored.
 */
void
gowl_input_recorder_add_deny_patterns(GowlInputRecorder  *self,
                                      const gchar *const *patterns)
{
	GPtrArray *merged;
	gsize      i;

	g_return_if_fail(GOWL_IS_INPUT_RECORDER(self));

	if (patterns == NULL)
		return;

	merged = g_ptr_array_new();
	if (self->deny_patterns != NULL) {
		for (i = 0; self->deny_patterns[i] != NULL; i++)
			g_ptr_array_add(merged, self->deny_patterns[i]);
	}

	for (i = 0; patterns[i] != NULL; i++) {
		gboolean dup = FALSE;
		guint    j;

		if (patterns[i][0] == '\0')
			continue;
		for (j = 0; j < merged->len; j++) {
			if (g_strcmp0((const gchar *)merged->pdata[j],
			              patterns[i]) == 0) {
				dup = TRUE;
				break;
			}
		}
		if (!dup)
			g_ptr_array_add(merged, (gpointer)patterns[i]);
	}

	g_ptr_array_add(merged, NULL);

	{
		gchar **combined;

		combined = g_strdupv((gchar **)merged->pdata);
		g_clear_pointer(&self->deny_patterns, g_strfreev);
		self->deny_patterns = combined;
	}

	g_ptr_array_free(merged, TRUE);
	rebuild_deny_specs(self);
}

/**
 * gowl_input_recorder_get_deny_patterns:
 * @self: a #GowlInputRecorder
 *
 * Returns the deny list currently in force.
 *
 * Returns: (transfer full) (array zero-terminated=1): the patterns
 */
gchar **
gowl_input_recorder_get_deny_patterns(GowlInputRecorder *self)
{
	g_return_val_if_fail(GOWL_IS_INPUT_RECORDER(self), NULL);

	return g_strdupv(self->deny_patterns);
}

/**
 * gowl_input_recorder_suppress_reason:
 * @self: a #GowlInputRecorder
 * @locked: whether the session is locked
 * @app_id: (nullable): app-id of the client holding keyboard focus
 * @title: (nullable): title of the client holding keyboard focus
 *
 * Decides whether input in the current context must not be recorded,
 * from the three facts a Wayland compositor actually has.  Pure: no
 * compositor is needed to test it.
 *
 * The lock screen is the only case gowl can be certain about.  A
 * credential prompt inside an ordinary client is recognised by app-id
 * or title and nothing more -- Wayland gives a compositor no
 * window-internal knowledge, so there is no equivalent of GNOME Shell's
 * "the focused actor is a Clutter.Text with password_char set".  A
 * prompt this list does not name **is recorded**.
 *
 * Returns: (nullable) (transfer none): a static reason string, or %NULL
 *   when the context is not known to be sensitive
 */
const gchar *
gowl_input_recorder_suppress_reason(GowlInputRecorder *self,
                                    gboolean           locked,
                                    const gchar       *app_id,
                                    const gchar       *title)
{
	g_return_val_if_fail(GOWL_IS_INPUT_RECORDER(self), "invalid recorder");

	if (locked)
		return "the session is locked";

	if (matches_deny(self, app_id))
		return "the focused window's app-id is on the deny list";

	if (matches_deny(self, title))
		return "the focused window's title is on the deny list";

	return NULL;
}

/* ---------------------------------------------------------------
 * Lifecycle
 * --------------------------------------------------------------- */

/**
 * gowl_input_recorder_start:
 * @self: a #GowlInputRecorder
 * @max_seconds: self-stop deadline in seconds; 0 for the default
 * @max_events: ring size in events; 0 for the default
 * @error: (out) (optional): return location for a #GError
 *
 * Starts a recording and returns its token.  Refuses when consent has
 * not been given, and refuses -- naming the running token -- when a
 * recording is already in progress, rather than silently taking it
 * over.
 *
 * @max_seconds and @max_events are clamped to the module's ceilings.
 * The deadline exists because a recording somebody forgot is a
 * recording of whatever they did next.
 *
 * Returns: (transfer full) (nullable): the token, or %NULL on error
 */
gchar *
gowl_input_recorder_start(GowlInputRecorder *self,
                          guint              max_seconds,
                          guint              max_events,
                          GError           **error)
{
	gchar *token;

	g_return_val_if_fail(GOWL_IS_INPUT_RECORDER(self), NULL);

	if (!self->consent) {
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
			"Input recording is not enabled.  It is a separate "
			"permission from input injection and is off by "
			"default; set `input-recording: true` in the gowl "
			"config and reload to allow it.");
		return NULL;
	}

	if (self->active) {
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_BUSY,
			"A recording is already running (token %s).  Stop it "
			"before starting another.",
			self->token != NULL ? self->token : "?");
		return NULL;
	}

	if (max_seconds == 0)
		max_seconds = GOWL_INPUT_RECORDER_DEFAULT_MAX_SECONDS;
	if (max_seconds > GOWL_INPUT_RECORDER_MAX_SECONDS_CEILING)
		max_seconds = GOWL_INPUT_RECORDER_MAX_SECONDS_CEILING;

	if (max_events == 0)
		max_events = GOWL_INPUT_RECORDER_DEFAULT_MAX_EVENTS;
	if (max_events > GOWL_INPUT_RECORDER_MAX_EVENTS_CEILING)
		max_events = GOWL_INPUT_RECORDER_MAX_EVENTS_CEILING;

	token = new_token();
	if (token == NULL) {
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED,
			"Could not read /dev/urandom for a recording token");
		return NULL;
	}

	reset_recording(self);

	self->ring = g_new0(GowlRecordedEvent, max_events);
	self->capacity        = max_events;
	self->max_seconds     = max_seconds;
	self->started_wall_us = g_get_real_time();
	self->started_mono_us = g_get_monotonic_time();
	self->deadline_us     = self->started_mono_us
	                        + (gint64)max_seconds * G_USEC_PER_SEC;

	g_clear_pointer(&self->token, g_free);
	self->token       = token;
	self->active      = TRUE;
	self->stop_reason = NULL;

	g_signal_emit(self, signals[SIGNAL_CHANGED], 0, TRUE, self->token);

	return g_strdup(self->token);
}

/**
 * gowl_input_recorder_is_active:
 * @self: a #GowlInputRecorder
 *
 * Returns whether a recording is running.  Checks the deadline first,
 * so a caller polling this sees a lapsed recording as stopped even if
 * no timer fired and no further input arrived.
 *
 * Returns: %TRUE if recording
 */
gboolean
gowl_input_recorder_is_active(GowlInputRecorder *self)
{
	g_return_val_if_fail(GOWL_IS_INPUT_RECORDER(self), FALSE);

	gowl_input_recorder_check_expiry(self);
	return self->active;
}

/**
 * gowl_input_recorder_get_token:
 * @self: a #GowlInputRecorder
 *
 * Returns the running recording's token.
 *
 * Returns: (nullable) (transfer none): the token, or %NULL
 */
const gchar *
gowl_input_recorder_get_token(GowlInputRecorder *self)
{
	g_return_val_if_fail(GOWL_IS_INPUT_RECORDER(self), NULL);

	return self->active ? self->token : NULL;
}

/**
 * gowl_input_recorder_get_deadline_us:
 * @self: a #GowlInputRecorder
 *
 * Returns the monotonic time at which the running recording stops
 * itself, or 0 when nothing is running.  The compositor uses this to
 * arm a real timer, so the recording stops even if nobody touches the
 * machine again.
 *
 * Returns: the deadline in monotonic microseconds, or 0
 */
gint64
gowl_input_recorder_get_deadline_us(GowlInputRecorder *self)
{
	g_return_val_if_fail(GOWL_IS_INPUT_RECORDER(self), 0);

	return self->active ? self->deadline_us : 0;
}

/**
 * gowl_input_recorder_check_expiry:
 * @self: a #GowlInputRecorder
 *
 * Stops the recording if its deadline has passed.  This is the backstop
 * behind the compositor's timer: every entry point calls it, so a
 * recording cannot outlive its deadline even on a path where the timer
 * never ran.  The buffered events are kept, so a final drain still
 * returns them.
 *
 * Returns: %TRUE if this call stopped the recording
 */
gboolean
gowl_input_recorder_check_expiry(GowlInputRecorder *self)
{
	g_return_val_if_fail(GOWL_IS_INPUT_RECORDER(self), FALSE);

	if (!self->active || self->deadline_us == 0)
		return FALSE;
	if (g_get_monotonic_time() < self->deadline_us)
		return FALSE;

	self->active      = FALSE;
	self->stop_reason = "the maximum recording time elapsed";
	g_signal_emit(self, signals[SIGNAL_CHANGED], 0, FALSE, self->token);

	return TRUE;
}

/**
 * gowl_input_recorder_force_stop:
 * @self: a #GowlInputRecorder
 * @reason: (nullable): a short static reason, shown in the next status
 *
 * Stops the running recording without a token.  This is the way out for
 * somebody who did not start the recording and does not have its
 * handle: the compositor binds it to Super+Shift+Escape, and
 * withdrawing consent calls it.  Buffered events are kept so a final
 * drain by whoever did start it still works.
 */
void
gowl_input_recorder_force_stop(GowlInputRecorder *self, const gchar *reason)
{
	g_return_if_fail(GOWL_IS_INPUT_RECORDER(self));

	if (!self->active)
		return;

	self->active      = FALSE;
	self->stop_reason = reason != NULL ? reason : "stopped";
	g_signal_emit(self, signals[SIGNAL_CHANGED], 0, FALSE, self->token);
}

/* ---------------------------------------------------------------
 * The tap
 * --------------------------------------------------------------- */

/*
 * newest:
 *
 * The most recently stored ring entry, or NULL when the ring is empty.
 */
static GowlRecordedEvent *
newest(GowlInputRecorder *self)
{
	guint idx;

	if (self->count == 0)
		return NULL;

	idx = (self->head + self->count - 1) % self->capacity;
	return &self->ring[idx];
}

/**
 * gowl_input_recorder_note:
 * @self: a #GowlInputRecorder
 * @event: (not nullable): the event to record; copied
 * @suppress_reason: (nullable): why this event must not be recorded
 *
 * Records one observed input event.  Called from the compositor's input
 * hooks on the compositor thread.  Does nothing unless a recording is
 * running.
 *
 * When @suppress_reason is non-%NULL the event is *not* stored; the
 * reason is remembered and the suppressed counter is incremented, so
 * the trace says plainly that something was withheld and why.  A silent
 * suppression would be indistinguishable from a gap in the
 * demonstration.
 *
 * @event's wall_us and offset_us are filled in here rather than by the
 * caller, so every entry is stamped from the same two clocks.
 */
void
gowl_input_recorder_note(GowlInputRecorder       *self,
                         const GowlRecordedEvent *event,
                         const gchar             *suppress_reason)
{
	GowlRecordedEvent *slot;
	GowlRecordedEvent *prev;
	gint64             now_mono;
	guint              idx;

	g_return_if_fail(GOWL_IS_INPUT_RECORDER(self));
	g_return_if_fail(event != NULL);

	if (!self->active)
		return;
	if (gowl_input_recorder_check_expiry(self))
		return;

	if (suppress_reason != NULL) {
		self->suppressed++;
		self->suppressed_total++;
		if (!g_hash_table_contains(self->suppress_reasons,
		                           suppress_reason))
			g_hash_table_add(self->suppress_reasons,
			                 g_strdup(suppress_reason));
		return;
	}

	now_mono = g_get_monotonic_time();

	/* Coalesce a run of pointer motion into one entry.  Not a drop:
	 * the accumulated delta and the later position are both kept, and
	 * the merge count is reported.  Without this a single drag fills
	 * the ring and the clicks the demonstration is about fall out of
	 * the far end. */
	if (event->type == GOWL_RECORDED_EVENT_POINTER_MOTION) {
		prev = newest(self);
		if (prev != NULL
		    && prev->type == GOWL_RECORDED_EVENT_POINTER_MOTION
		    && now_mono - (self->started_mono_us + prev->offset_us)
		       < GOWL_INPUT_RECORDER_MOTION_COALESCE_US) {
			prev->dx       += event->dx;
			prev->dy       += event->dy;
			prev->x         = event->x;
			prev->y         = event->y;
			prev->wall_us   = g_get_real_time();
			prev->offset_us = now_mono - self->started_mono_us;
			prev->merged++;
			self->coalesced++;
			self->coalesced_total++;
			return;
		}
	}

	if (self->count == self->capacity) {
		/* Ring full: overwrite the oldest and say so.  The
		 * alternative -- growing -- is an unbounded allocation
		 * inside the compositor process, which is a way to lose a
		 * session rather than a way to keep a demonstration. */
		self->head = (self->head + 1) % self->capacity;
		self->count--;
		self->dropped++;
		self->dropped_total++;
	}

	idx  = (self->head + self->count) % self->capacity;
	slot = &self->ring[idx];

	*slot = *event;
	slot->wall_us   = g_get_real_time();
	slot->offset_us = now_mono - self->started_mono_us;
	slot->merged    = 0;

	self->count++;
}

/* ---------------------------------------------------------------
 * Rendering
 * --------------------------------------------------------------- */

static const gchar *
event_type_name(GowlRecordedEventType type)
{
	switch (type) {
	case GOWL_RECORDED_EVENT_KEY:
		return "key";
	case GOWL_RECORDED_EVENT_MODIFIERS:
		return "modifiers";
	case GOWL_RECORDED_EVENT_POINTER_MOTION:
		return "pointer_motion";
	case GOWL_RECORDED_EVENT_POINTER_BUTTON:
		return "pointer_button";
	case GOWL_RECORDED_EVENT_POINTER_AXIS:
		return "pointer_axis";
	}

	return "unknown";
}

/*
 * add_event:
 *
 * Renders one ring entry.  Only the members that mean something for the
 * entry's type are emitted, so a consumer cannot read a zero as a
 * measurement.
 *
 * Note what is *not* here: the compositor's own event timestamp.  See
 * the GowlRecordedEvent doc comment.
 */
static void
add_event(JsonBuilder *b, const GowlRecordedEvent *e)
{
	json_builder_begin_object(b);

	json_builder_set_member_name(b, "type");
	json_builder_add_string_value(b, event_type_name(e->type));

	json_builder_set_member_name(b, "offset_ms");
	json_builder_add_double_value(b, (gdouble)e->offset_us / 1000.0);

	json_builder_set_member_name(b, "wall_us");
	json_builder_add_int_value(b, e->wall_us);

	switch (e->type) {
	case GOWL_RECORDED_EVENT_KEY: {
		gchar name[64];

		json_builder_set_member_name(b, "keycode");
		json_builder_add_int_value(b, e->keycode);
		json_builder_set_member_name(b, "keysym");
		if (e->keysym != 0
		    && xkb_keysym_get_name((xkb_keysym_t)e->keysym,
		                           name, sizeof name) > 0)
			json_builder_add_string_value(b, name);
		else
			json_builder_add_null_value(b);
		json_builder_set_member_name(b, "state");
		json_builder_add_string_value(b,
			e->state ? "press" : "release");
		json_builder_set_member_name(b, "mods");
		json_builder_add_int_value(b, e->mods);
		break;
	}
	case GOWL_RECORDED_EVENT_MODIFIERS:
		json_builder_set_member_name(b, "depressed");
		json_builder_add_int_value(b, e->mods_depressed);
		json_builder_set_member_name(b, "latched");
		json_builder_add_int_value(b, e->mods_latched);
		json_builder_set_member_name(b, "locked");
		json_builder_add_int_value(b, e->mods_locked);
		json_builder_set_member_name(b, "group");
		json_builder_add_int_value(b, e->mods_group);
		break;
	case GOWL_RECORDED_EVENT_POINTER_MOTION:
		json_builder_set_member_name(b, "x");
		json_builder_add_double_value(b, e->x);
		json_builder_set_member_name(b, "y");
		json_builder_add_double_value(b, e->y);
		json_builder_set_member_name(b, "dx");
		json_builder_add_double_value(b, e->dx);
		json_builder_set_member_name(b, "dy");
		json_builder_add_double_value(b, e->dy);
		json_builder_set_member_name(b, "merged");
		json_builder_add_int_value(b, e->merged);
		break;
	case GOWL_RECORDED_EVENT_POINTER_BUTTON:
		json_builder_set_member_name(b, "button");
		json_builder_add_int_value(b, e->button);
		json_builder_set_member_name(b, "state");
		json_builder_add_string_value(b,
			e->state ? "press" : "release");
		json_builder_set_member_name(b, "x");
		json_builder_add_double_value(b, e->x);
		json_builder_set_member_name(b, "y");
		json_builder_add_double_value(b, e->y);
		json_builder_set_member_name(b, "mods");
		json_builder_add_int_value(b, e->mods);
		break;
	case GOWL_RECORDED_EVENT_POINTER_AXIS:
		json_builder_set_member_name(b, "axis");
		json_builder_add_string_value(b,
			e->axis == 1 ? "horizontal" : "vertical");
		json_builder_set_member_name(b, "value");
		json_builder_add_double_value(b, e->value);
		json_builder_set_member_name(b, "discrete");
		json_builder_add_int_value(b, e->discrete);
		json_builder_set_member_name(b, "x");
		json_builder_add_double_value(b, e->x);
		json_builder_set_member_name(b, "y");
		json_builder_add_double_value(b, e->y);
		break;
	}

	json_builder_end_object(b);
}

/*
 * add_common:
 *
 * The members every payload carries, so a consumer parses one shape
 * whether it called drain, stop or status.
 */
static void
add_common(GowlInputRecorder *self, JsonBuilder *b)
{
	GHashTableIter iter;
	gpointer       key;

	json_builder_set_member_name(b, "token");
	if (self->token != NULL)
		json_builder_add_string_value(b, self->token);
	else
		json_builder_add_null_value(b);

	json_builder_set_member_name(b, "active");
	json_builder_add_boolean_value(b, self->active);

	json_builder_set_member_name(b, "consent");
	json_builder_add_boolean_value(b, self->consent);

	json_builder_set_member_name(b, "started_wall_us");
	json_builder_add_int_value(b, self->started_wall_us);

	json_builder_set_member_name(b, "max_seconds");
	json_builder_add_int_value(b, self->max_seconds);

	json_builder_set_member_name(b, "max_events");
	json_builder_add_int_value(b, self->capacity);

	json_builder_set_member_name(b, "buffered");
	json_builder_add_int_value(b, self->count);

	json_builder_set_member_name(b, "dropped_total");
	json_builder_add_int_value(b, (gint64)self->dropped_total);

	json_builder_set_member_name(b, "suppressed_total");
	json_builder_add_int_value(b, (gint64)self->suppressed_total);

	json_builder_set_member_name(b, "coalesced_total");
	json_builder_add_int_value(b, (gint64)self->coalesced_total);

	json_builder_set_member_name(b, "suppress_reasons");
	json_builder_begin_array(b);
	g_hash_table_iter_init(&iter, self->suppress_reasons);
	while (g_hash_table_iter_next(&iter, &key, NULL))
		json_builder_add_string_value(b, (const gchar *)key);
	json_builder_end_array(b);

	json_builder_set_member_name(b, "stop_reason");
	if (self->stop_reason != NULL)
		json_builder_add_string_value(b, self->stop_reason);
	else
		json_builder_add_null_value(b);

	/* Repeated in every payload on purpose.  A reviewer reading a
	 * trace in a chat window needs to know the guard's actual reach at
	 * the point they are reading it, not only in a document. */
	json_builder_set_member_name(b, "secret_suppression");
	json_builder_add_string_value(b,
		"gowl suppresses capture while the session is locked and "
		"while the focused window's app-id or title matches the "
		"deny list.  Wayland gives a compositor no window-internal "
		"knowledge, so a password field inside an ordinary window "
		"that the deny list does not name IS recorded.  Review a "
		"trace before it is stored or shared.");

	json_builder_set_member_name(b, "generated_wall_us");
	json_builder_add_int_value(b, g_get_real_time());
}

/*
 * build_payload:
 *
 * Renders the common members, plus the buffered events when @with_events.
 * When events are included the "since the last drain" counters are
 * reported and then cleared, and the ring is emptied -- so a consumer
 * that drains in a loop can sum them and get the true totals, and a
 * consumer that sums the *_total members gets them directly.
 */
static gchar *
build_payload(GowlInputRecorder *self, gboolean with_events)
{
	g_autoptr(JsonBuilder) b    = NULL;
	g_autoptr(JsonNode)    root = NULL;
	g_autoptr(JsonGenerator) gen = NULL;
	guint i;

	b = json_builder_new();
	json_builder_begin_object(b);

	add_common(self, b);

	if (with_events) {
		json_builder_set_member_name(b, "dropped");
		json_builder_add_int_value(b, (gint64)self->dropped);
		json_builder_set_member_name(b, "suppressed");
		json_builder_add_int_value(b, (gint64)self->suppressed);
		json_builder_set_member_name(b, "coalesced");
		json_builder_add_int_value(b, (gint64)self->coalesced);

		json_builder_set_member_name(b, "events");
		json_builder_begin_array(b);
		for (i = 0; i < self->count; i++) {
			guint idx = (self->head + i) % self->capacity;

			add_event(b, &self->ring[idx]);
		}
		json_builder_end_array(b);
	}

	json_builder_end_object(b);

	if (with_events) {
		self->head       = 0;
		self->count      = 0;
		self->dropped    = 0;
		self->suppressed = 0;
		self->coalesced  = 0;
	}

	root = json_builder_get_root(b);
	gen  = json_generator_new();
	json_generator_set_root(gen, root);

	return json_generator_to_data(gen, NULL);
}

/*
 * check_token:
 *
 * A drain or a stop must name the recording it means.  A caller holding
 * a stale token is asking about a recording that has been replaced, and
 * answering with the current one's events would hand it somebody else's
 * keystrokes.
 */
static gboolean
check_token(GowlInputRecorder *self, const gchar *token, GError **error)
{
	if (self->token == NULL) {
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
			"No recording has been started");
		return FALSE;
	}

	if (g_strcmp0(self->token, token) != 0) {
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
			"Unknown recording token");
		return FALSE;
	}

	return TRUE;
}

/**
 * gowl_input_recorder_drain:
 * @self: a #GowlInputRecorder
 * @token: the token returned by gowl_input_recorder_start()
 * @error: (out) (optional): return location for a #GError
 *
 * Returns everything buffered since the last drain as a JSON object and
 * empties the ring, leaving the recording running.  The reply carries
 * `dropped` (since the last drain) and `dropped_total` (this recording),
 * so a truncated demonstration is reported rather than passed off as a
 * complete one.
 *
 * Draining a recording that has already stopped is allowed and returns
 * the tail: the deadline stops the capture, not the reader.
 *
 * Returns: (transfer full) (nullable): the JSON payload, or %NULL on error
 */
gchar *
gowl_input_recorder_drain(GowlInputRecorder *self,
                          const gchar       *token,
                          GError           **error)
{
	g_return_val_if_fail(GOWL_IS_INPUT_RECORDER(self), NULL);

	gowl_input_recorder_check_expiry(self);

	if (!check_token(self, token, error))
		return NULL;

	return build_payload(self, TRUE);
}

/**
 * gowl_input_recorder_stop:
 * @self: a #GowlInputRecorder
 * @token: the token returned by gowl_input_recorder_start()
 * @error: (out) (optional): return location for a #GError
 *
 * Stops the recording and returns the tail, in the same shape drain
 * uses.  Stopping an already-stopped recording still returns its
 * remaining tail exactly once, because the deadline may have stopped it
 * before the caller got here.
 *
 * Returns: (transfer full) (nullable): the JSON payload, or %NULL on error
 */
gchar *
gowl_input_recorder_stop(GowlInputRecorder *self,
                         const gchar       *token,
                         GError           **error)
{
	g_return_val_if_fail(GOWL_IS_INPUT_RECORDER(self), NULL);

	gowl_input_recorder_check_expiry(self);

	if (!check_token(self, token, error))
		return NULL;

	if (self->active) {
		self->active      = FALSE;
		self->stop_reason = "stopped by the caller";
		g_signal_emit(self, signals[SIGNAL_CHANGED], 0,
		              FALSE, self->token);
	}

	return build_payload(self, TRUE);
}

/**
 * gowl_input_recorder_status:
 * @self: a #GowlInputRecorder
 *
 * Returns the recorder's state as JSON, without events and without
 * consuming anything.  This is what a consumer polls, and what the MCP
 * module publishes as a resource so a consumer can be told rather than
 * having to ask.
 *
 * Returns: (transfer full): the JSON payload
 */
gchar *
gowl_input_recorder_status(GowlInputRecorder *self)
{
	g_return_val_if_fail(GOWL_IS_INPUT_RECORDER(self), NULL);

	gowl_input_recorder_check_expiry(self);

	return build_payload(self, FALSE);
}
