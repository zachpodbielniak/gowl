/*
 * gowl - GObject Wayland Compositor
 * Copyright (C) 2026  Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * bar-meeting-model.h -- the pure half of the `meeting-notes' widget.
 *
 * Everything here is text in, text out: reading `pactl -f json list
 * sources', turning a command template into an argv, naming files,
 * splitting a model's summary into its sections and assembling the
 * notes document.  None of it spawns, touches the compositor, the theme
 * or a GowlBarPlugin, so tests/test-bar-meeting-notes.c drives every
 * function against the exact input that once went wrong.
 */

#ifndef BAR_MEETING_MODEL_H
#define BAR_MEETING_MODEL_H

#include <glib.h>

G_BEGIN_DECLS

/**
 * BAR_MEETING_MAX_SOURCES:
 *
 * The most sources one listing is allowed to yield.  A real machine has
 * a dozen; a listing with thousands is hostile or broken, and every one
 * would become a panel row.
 */
#define BAR_MEETING_MAX_SOURCES (64)

/**
 * BAR_MEETING_MAX_NAME:
 *
 * The longest source name accepted, in bytes.  PipeWire names run to
 * about eighty; anything past this is not a name.
 */
#define BAR_MEETING_MAX_NAME (256)

/**
 * BarMeetingPhase:
 * @BAR_MEETING_IDLE: nothing happening
 * @BAR_MEETING_STARTING: recorders are being spawned on a worker
 * @BAR_MEETING_RECORDING: recorders running
 * @BAR_MEETING_STOPPING: SIGINT sent, waiting for the recorders to exit
 * @BAR_MEETING_MIXING: the recordings are being mixed into one file
 * @BAR_MEETING_TRANSCRIBING: speech to text
 * @BAR_MEETING_SUMMARISING: a model is summarising the transcript
 * @BAR_MEETING_FILING: the notes file is being written (or, under
 *   cmacs, routed to its destination)
 * @BAR_MEETING_DONE: the last meeting was filed
 * @BAR_MEETING_ERROR: the last meeting failed; the error says why
 *
 * Where one meeting is in its life.  Both backends walk the same
 * phases, which is what lets the bar show one vocabulary whether the
 * work happens in this process or in cmacs.
 */
typedef enum {
	BAR_MEETING_IDLE = 0,
	BAR_MEETING_STARTING,
	BAR_MEETING_RECORDING,
	BAR_MEETING_STOPPING,
	BAR_MEETING_MIXING,
	BAR_MEETING_TRANSCRIBING,
	BAR_MEETING_SUMMARISING,
	BAR_MEETING_FILING,
	BAR_MEETING_DONE,
	BAR_MEETING_ERROR
} BarMeetingPhase;

/**
 * bar_meeting_phase_name:
 * @phase: a phase
 *
 * Returns: (transfer none): the phase's name, as cmacs spells it in
 *   `meeting-notes.state'
 */
const gchar *bar_meeting_phase_name (BarMeetingPhase phase);

/**
 * bar_meeting_phase_from_name:
 * @name: (nullable): a state name pushed by cmacs
 * @phase: (out): the phase it names
 *
 * Accepts both spellings of `summarising', since the cmacs side is
 * written by people who use either.
 *
 * Returns: %TRUE when @name is a state cmacs may push
 */
gboolean bar_meeting_phase_from_name (const gchar     *name,
                                      BarMeetingPhase *phase);

/**
 * bar_meeting_phase_is_busy:
 * @phase: a phase
 *
 * Returns: %TRUE while a meeting is being recorded or processed, which
 *   is when the bar polls once a second and refuses a second start
 */
gboolean bar_meeting_phase_is_busy (BarMeetingPhase phase);

/**
 * BarMeetingSource:
 * @name: the PipeWire/Pulse source name, what a recorder is given
 * @description: the human name
 * @monitor: %TRUE for an output's monitor (what the speakers play),
 *   %FALSE for a microphone or line input
 *
 * One recordable source.
 */
typedef struct {
	gchar    *name;
	gchar    *description;
	gboolean  monitor;
} BarMeetingSource;

/**
 * bar_meeting_source_new:
 * @name: a source name
 * @description: (nullable): its human name; @name when %NULL
 * @monitor: whether it is a monitor
 *
 * Returns: (transfer full): a new source
 */
BarMeetingSource *bar_meeting_source_new  (const gchar *name,
                                           const gchar *description,
                                           gboolean     monitor);
void              bar_meeting_source_free (BarMeetingSource *source);

/**
 * bar_meeting_name_is_valid:
 * @name: (nullable): a source name from pactl or the state file
 *
 * Whether @name may be handed to a recorder as one argv element: valid
 * UTF-8, no control characters, not empty, not longer than
 * %BAR_MEETING_MAX_NAME, and not starting with `-' --- a source named
 * `--output=/home/x/.bashrc' would otherwise be read as an option.
 *
 * Returns: %TRUE when @name is safe to record from
 */
gboolean bar_meeting_name_is_valid (const gchar *name);

/**
 * bar_meeting_parse_sources:
 * @json: (nullable): the stdout of `pactl -f json list sources'
 *
 * Reads the source list.  A source is a monitor when its name ends in
 * `.monitor' or its `device.class' property says `monitor'.  Fields are
 * read from a SHALLOW copy of each object --- nested objects and arrays
 * blanked --- so the `name' of a port inside `ports' can never be taken
 * for the source's own.  Invalid names are dropped, descriptions are
 * made valid UTF-8, and at most %BAR_MEETING_MAX_SOURCES are kept.
 *
 * Returns: (transfer full) (element-type BarMeetingSource): the
 *   sources, monitors first; never %NULL
 */
GPtrArray *bar_meeting_parse_sources (const gchar *json);

/**
 * bar_meeting_default_selection:
 * @default_sink: (nullable): `pactl get-default-sink'
 * @default_source: (nullable): `pactl get-default-source'
 *
 * What to record when the user has never chosen: what the speakers play
 * (the default sink's monitor) and the default microphone.  A default
 * source that is itself a monitor is not doubled up.
 *
 * Returns: (transfer full): a %NULL-terminated list of source names
 */
gchar **bar_meeting_default_selection (const gchar *default_sink,
                                       const gchar *default_source);

/**
 * bar_meeting_parse_selection:
 * @text: (nullable): the contents of the selection state file
 *
 * One source name per line; blank lines and `#' comments skipped,
 * invalid names dropped, duplicates removed.
 *
 * Returns: (transfer full): a %NULL-terminated list, possibly empty
 */
gchar **bar_meeting_parse_selection (const gchar *text);

/**
 * bar_meeting_format_selection:
 * @names: (array zero-terminated=1) (nullable): source names
 *
 * Returns: (transfer full): the state-file text for @names
 */
gchar *bar_meeting_format_selection (const gchar * const *names);

/**
 * bar_meeting_echo_risk:
 * @selected: (element-type BarMeetingSource): what will be recorded
 *
 * A cheap guess at whether one voice will be recorded twice: a
 * microphone and a monitor of the same sound card (the laptop's
 * speakers into the laptop's microphone), or any monitor of a sink
 * whose name says `Speaker' alongside any microphone.  Headphones do
 * not leak; speakers do.  It is a heuristic and the toast it backs says
 * so.
 *
 * Returns: %TRUE when the selection probably records echo
 */
gboolean bar_meeting_echo_risk (GPtrArray *selected);

/**
 * bar_meeting_expand_template:
 * @tmpl: a command template, e.g. `pw-record --target {source} {output}'
 * @pairs: (array zero-terminated=1): placeholder names and values,
 *   alternating: `"source", name, "output", path, NULL'
 * @error: (out) (optional): why the template could not be split
 *
 * Splits @tmpl into words with shell QUOTING rules (never through a
 * shell: no pipes, no variables, no globbing), then substitutes each
 * `{name}' inside each word.  A value is inserted as text into the one
 * word that named it, so a path with spaces or quotes stays a single
 * argument, and a value is never itself searched for placeholders.  An
 * unknown `{name}' is left as written.
 *
 * Returns: (transfer full) (nullable): the argv, or %NULL with @error
 *   set when @tmpl is empty or has unbalanced quotes
 */
gchar **bar_meeting_expand_template (const gchar         *tmpl,
                                     const gchar * const *pairs,
                                     GError             **error);

/**
 * bar_meeting_mix_argv:
 * @ffmpeg: (nullable): the ffmpeg executable; `ffmpeg' when %NULL
 * @inputs: (array zero-terminated=1): the recordings
 * @output: the mixed file to write
 *
 * The ffmpeg call that mixes several recordings into one 16 kHz mono
 * WAV, which is what whisper wants.  `normalize=0' keeps each input at
 * its own level: amix's default divides every input by the input
 * count, which turns a quiet remote voice inaudible.
 *
 * Returns: (transfer full) (nullable): the argv, or %NULL for fewer
 *   than two inputs (one recording needs no mixing)
 */
gchar **bar_meeting_mix_argv (const gchar         *ffmpeg,
                              const gchar * const *inputs,
                              const gchar         *output);

/**
 * bar_meeting_format_clock:
 * @seconds: an elapsed time
 *
 * Returns: (transfer full): `mm:ss', or `h:mm:ss' from an hour on
 */
gchar *bar_meeting_format_clock (gint64 seconds);

/**
 * bar_meeting_slug:
 * @title: (nullable): a meeting title the user typed
 *
 * Lower-case ASCII letters and digits joined by single underscores,
 * at most 40 characters: safe in a file name on any filesystem and in
 * a shell, whatever was typed.
 *
 * Returns: (transfer full): the slug, possibly empty
 */
gchar *bar_meeting_slug (const gchar *title);

/**
 * bar_meeting_session_name:
 * @when: the meeting's start
 *
 * Returns: (transfer full): `YYYY-MM-DD_HH-MM-SS', the session directory
 */
gchar *bar_meeting_session_name (GDateTime *when);

/**
 * bar_meeting_recording_filename:
 * @index: the source's position, from zero
 * @source: the source name
 *
 * `01-<sanitised name>.wav'.  The index keeps two sources whose names
 * sanitise alike from writing the same file.
 *
 * Returns: (transfer full): a file name with no directory part
 */
gchar *bar_meeting_recording_filename (guint        index,
                                       const gchar *source);

/**
 * bar_meeting_notes_filename:
 * @when: the meeting's start
 * @title: (nullable): its title
 * @format: `org' or `md'
 *
 * Returns: (transfer full): `YYYY-MM-DD_HH-MM_meeting[_slug].org'
 */
gchar *bar_meeting_notes_filename (GDateTime   *when,
                                   const gchar *title,
                                   const gchar *format);

/**
 * bar_meeting_default_title:
 * @when: the meeting's start
 *
 * Returns: (transfer full): `Meeting YYYY-MM-DD HH:MM'
 */
gchar *bar_meeting_default_title (GDateTime *when);

/**
 * bar_meeting_split_summary:
 * @summary: (nullable): a model's summary, Org or Markdown
 * @actions: (out) (transfer full) (nullable): the body of its `Action
 *   items' section, or %NULL when it has none
 *
 * Takes the action items out of a summary so the notes can give them a
 * section of their own, and drops a heading that only says `Summary'
 * (or `TL;DR'), which the notes already provide.  Every other heading is
 * kept, in whichever syntax the model used.
 *
 * Returns: (transfer full): the rest of the summary, trimmed
 */
gchar *bar_meeting_split_summary (const gchar  *summary,
                                  gchar       **actions);

/**
 * BarMeetingDoc:
 * @format: `org' (anything else is Markdown)
 * @title: the meeting title
 * @created: when the meeting started
 * @author: (nullable): the #+authors value
 * @summary: (nullable): the model's summary
 * @summary_note: (nullable): why there is no summary, shown in its place
 * @transcript: (nullable): the transcript
 * @duration_s: the recording's length
 * @n_sources: how many sources were recorded
 * @recordings: (nullable): where the recordings were kept
 *
 * Everything the notes document is assembled from.
 */
typedef struct {
	const gchar *format;
	const gchar *title;
	GDateTime   *created;
	const gchar *author;
	const gchar *summary;
	const gchar *summary_note;
	const gchar *transcript;
	gint64       duration_s;
	guint        n_sources;
	const gchar *recordings;
} BarMeetingDoc;

/**
 * bar_meeting_document:
 * @doc: the parts
 *
 * The notes file: a header in the notes repository's convention
 * (#+title, #+description, #+authors, #+categories, #+created,
 * #+updated, #+version, ISO 8601 with offset), or YAML front matter for
 * Markdown, then the sections Summary, Action items, Transcript and My
 * notes.  Headings inside the summary are re-levelled under Summary in
 * the output syntax; a transcript line that would read as a heading or
 * an Org keyword is escaped.
 *
 * Returns: (transfer full): the document
 */
gchar *bar_meeting_document (const BarMeetingDoc *doc);

/**
 * bar_meeting_cmacs_form:
 * @widget: the widget's registry name
 * @job_id: the id cmacs echoes in `<widget>.job-id'
 * @session_dir: the session directory
 * @files: (array zero-terminated=1): the recordings, relative to it
 * @title: (nullable): the meeting title; nil when empty
 * @destination: (nullable): where cmacs files it; nil for its default
 *
 * `(cmacs-gowl-bar-meeting-finish WIDGET ID DIR (list FILE...) TITLE
 * DESTINATION)', every string escaped as an Elisp literal.  %NULL when
 * any text is not UTF-8 or holds a NUL or control character other than
 * a tab, since neither survives D-Bus and a newline in a title is not a
 * title.
 *
 * Returns: (transfer full) (nullable): the form
 */
gchar *bar_meeting_cmacs_form (const gchar         *widget,
                               const gchar         *job_id,
                               const gchar         *session_dir,
                               const gchar * const *files,
                               const gchar         *title,
                               const gchar         *destination);

/**
 * bar_meeting_clean_text:
 * @text: (nullable): text from a process or a file
 * @max_bytes: the most to keep
 *
 * Makes @text valid UTF-8, drops NULs and cuts it at a character
 * boundary no later than @max_bytes.
 *
 * Returns: (transfer full): the cleaned text, never %NULL
 */
gchar *bar_meeting_clean_text (const gchar *text,
                               gsize        max_bytes);

G_END_DECLS

#endif /* BAR_MEETING_MODEL_H */
