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
 * The `meeting-notes' bar widget (modules/bar/bar-plugins-meeting.c and
 * bar-meeting-model.c), offline.
 *
 * The pure half is included directly and driven against a real,
 * sanitised `pactl -f json list sources' capture and against hostile
 * input.  The widget half is loaded from the real bar.so through
 * tests/bar-widget-harness.h, with every command it runs replaced by a
 * stand-in script: a pw-record that writes a WAV header and waits for
 * SIGINT, an ffmpeg, a whisper-cli and an `ai' that write what the real
 * ones would, an emacsctl that records the form it was handed.
 *
 * The children are REAL processes.  The lifecycle tests assert they got
 * SIGINT, were reaped (kill(pid, 0) fails with ESRCH --- a zombie would
 * still answer), and that nothing is left behind.
 */

#include <glib.h>
#include <glib/gstdio.h>
#include <errno.h>
#include <signal.h>
#include <string.h>
#include <sys/types.h>
#include <linux/input-event-codes.h>

#include "../modules/bar/bar-util.c"
#include "../modules/bar/bar-meeting-model.c"

#define BAR_HARNESS_MODULE GOWL_TEST_BAR_MODULE
#include "bar-widget-harness.h"

#ifndef GOWL_TEST_BAR_MODULE
#define GOWL_TEST_BAR_MODULE "build/release/modules/bar.so"
#endif
#ifndef GOWL_TEST_FIXTURES
#define GOWL_TEST_FIXTURES "tests/fixtures/bar"
#endif

#define FIX_DIR GOWL_TEST_FIXTURES "/meeting-notes"

#define SINK   "alsa_output.usb-Schiit_Audio_Schiit_Modi_3E-00.analog-stereo"
#define MIC    "alsa_input.pci-0000_c1_00.6.HiFi__Mic1__source"
#define SPEAKER_MON \
	"alsa_output.pci-0000_c1_00.6.HiFi__Speaker__sink.monitor"

static gchar *
fixture_text(const gchar *name)
{
	g_autofree gchar *path = g_build_filename(FIX_DIR, name, NULL);
	gchar *text = NULL;

	g_assert_true(g_file_get_contents(path, &text, NULL, NULL));
	return text;
}

static BarMeetingSource *
find_source(GPtrArray *sources, const gchar *name)
{
	guint i;

	for (i = 0; i < sources->len; i++) {
		BarMeetingSource *s = g_ptr_array_index(sources, i);

		if (strcmp(s->name, name) == 0)
			return s;
	}
	return NULL;
}

/* ══ The pure half ══════════════════════════════════════════════════ */

/*
 * The real capture parses into the seven sources it lists, monitors
 * first, each classified correctly.  Catches: a monitor taken for a
 * microphone (the default selection would then record the wrong thing)
 * and a port's `name' read in place of the source's own.
 */
static void
test_sources_from_real_pactl(void)
{
	g_autofree gchar *json = fixture_text("pactl-sources.json");
	g_autoptr(GPtrArray) sources = bar_meeting_parse_sources(json);
	BarMeetingSource *s;
	guint i;
	gboolean seen_mic = FALSE;

	g_assert_cmpuint(sources->len, ==, 7);
	/* Monitors first, then microphones, with no monitor after a mic. */
	for (i = 0; i < sources->len; i++) {
		s = g_ptr_array_index(sources, i);
		if (!s->monitor)
			seen_mic = TRUE;
		else
			g_assert_false(seen_mic);
	}

	s = find_source(sources, SINK ".monitor");
	g_assert_nonnull(s);
	g_assert_true(s->monitor);
	g_assert_cmpstr(s->description, ==,
	                "Monitor of Schiit Modi 3E Analog Stereo");

	s = find_source(sources, MIC);
	g_assert_nonnull(s);
	g_assert_false(s->monitor);
	g_assert_cmpstr(s->description, ==,
	                "Ryzen HD Audio Controller Digital Microphone");

	/* `dummy.monitor' is a null sink's monitor: still a monitor. */
	s = find_source(sources, "dummy.monitor");
	g_assert_nonnull(s);
	g_assert_true(s->monitor);

	/* No port name leaked in as a source. */
	for (i = 0; i < sources->len; i++) {
		s = g_ptr_array_index(sources, i);
		g_assert_false(g_str_has_prefix(s->name, "analog-"));
	}
}

/*
 * Empty, truncated, hostile and oversized listings.  Catches: a crash
 * or a hang on a truncated reply, a name starting with `-' reaching a
 * recorder as an option, non-UTF-8 reaching pango, a listing of
 * thousands becoming thousands of panel rows, and the ports-before-name
 * ordering that a forward search gets wrong.
 */
static void
test_sources_hostile(void)
{
	g_autoptr(GPtrArray) none = bar_meeting_parse_sources(NULL);
	g_autoptr(GPtrArray) empty = bar_meeting_parse_sources("[]");
	g_autoptr(GPtrArray) junk = bar_meeting_parse_sources("not json");
	g_autoptr(GPtrArray) cut = bar_meeting_parse_sources(
		"[{\"name\":\"ok.monitor\",\"description\":\"A\"},"
		"{\"name\":\"half");
	g_autoptr(GPtrArray) evil = NULL;
	g_autoptr(GPtrArray) many = NULL;
	g_autoptr(GPtrArray) order = NULL;
	GString *big;
	guint i;

	g_assert_cmpuint(none->len, ==, 0);
	g_assert_cmpuint(empty->len, ==, 0);
	g_assert_cmpuint(junk->len, ==, 0);
	g_assert_cmpuint(cut->len, ==, 1);

	evil = bar_meeting_parse_sources(
		"[{\"name\":\"--output=/home/x/.bashrc\",\"description\":\"x\"},"
		"{\"name\":\"bad\\nname\",\"description\":\"x\"},"
		"{\"name\":\"mic\",\"description\":\"caf\xff\xfe\"}]");
	g_assert_cmpuint(evil->len, ==, 1);
	g_assert_cmpstr(((BarMeetingSource *)
		g_ptr_array_index(evil, 0))->name, ==, "mic");
	g_assert_true(g_utf8_validate(((BarMeetingSource *)
		g_ptr_array_index(evil, 0))->description, -1, NULL));

	big = g_string_new("[");
	for (i = 0; i < 500; i++)
		g_string_append_printf(big, "%s{\"name\":\"s%u\"}",
		                       i ? "," : "", i);
	g_string_append_c(big, ']');
	many = bar_meeting_parse_sources(big->str);
	g_assert_cmpuint(many->len, ==, BAR_MEETING_MAX_SOURCES);
	g_string_free(big, TRUE);

	/* A port named before the source in the object, with a brace in a
	   description to try to unbalance the walk. */
	order = bar_meeting_parse_sources(
		"[{\"ports\":[{\"name\":\"analog-input-mic\","
		"\"description\":\"Mic }{ ]\"}],"
		"\"name\":\"real.source\",\"description\":\"Real\"}]");
	g_assert_cmpuint(order->len, ==, 1);
	g_assert_cmpstr(((BarMeetingSource *)
		g_ptr_array_index(order, 0))->name, ==, "real.source");
	g_assert_cmpstr(((BarMeetingSource *)
		g_ptr_array_index(order, 0))->description, ==, "Real");
}

/*
 * The defaults and the saved selection.  Catches: recording the same
 * monitor twice when the default source IS that monitor, and a state
 * file whose garbage lines reach a recorder.
 */
static void
test_selection(void)
{
	g_auto(GStrv) def = bar_meeting_default_selection(SINK, MIC);
	g_auto(GStrv) same = bar_meeting_default_selection(SINK,
	                                                   SINK ".monitor");
	g_auto(GStrv) nothing = bar_meeting_default_selection(NULL, "");
	g_auto(GStrv) parsed = NULL;
	g_auto(GStrv) again = NULL;
	g_autofree gchar *text = NULL;

	g_assert_cmpuint(g_strv_length(def), ==, 2);
	g_assert_cmpstr(def[0], ==, SINK ".monitor");
	g_assert_cmpstr(def[1], ==, MIC);
	g_assert_cmpuint(g_strv_length(same), ==, 1);
	g_assert_cmpuint(g_strv_length(nothing), ==, 0);

	parsed = bar_meeting_parse_selection(
		"# comment\n\n  " MIC "  \n" MIC "\n-rf\nbad\x01name\n"
		SINK ".monitor\n");
	g_assert_cmpuint(g_strv_length(parsed), ==, 2);
	g_assert_cmpstr(parsed[0], ==, MIC);

	text = bar_meeting_format_selection((const gchar * const *)parsed);
	again = bar_meeting_parse_selection(text);
	g_assert_true(g_strv_equal((const gchar * const *)parsed,
	                           (const gchar * const *)again));
}

/*
 * The echo heuristic.  Catches: no warning for the laptop's speakers
 * into the laptop's microphone (the case that matters), and a warning
 * for a USB DAC monitor beside an unrelated microphone (noise).
 */
static void
test_echo_risk(void)
{
	g_autoptr(GPtrArray) sel = g_ptr_array_new_with_free_func(
		(GDestroyNotify)bar_meeting_source_free);

	g_ptr_array_add(sel, bar_meeting_source_new(SINK ".monitor",
		"Monitor of Schiit Modi 3E Analog Stereo", TRUE));
	g_ptr_array_add(sel, bar_meeting_source_new(MIC, "Digital Mic",
	                                            FALSE));
	g_assert_false(bar_meeting_echo_risk(sel));

	g_ptr_array_add(sel, bar_meeting_source_new(SPEAKER_MON,
		"Monitor of Ryzen HD Audio Controller Speaker", TRUE));
	g_assert_true(bar_meeting_echo_risk(sel));

	/* Same card, no `Speaker' in the name: matched by device. */
	g_ptr_array_set_size(sel, 0);
	g_ptr_array_add(sel, bar_meeting_source_new(
		"alsa_output.usb-audio-technica_AT2020USB_-00.analog-stereo"
		".monitor", "Monitor of AT2020USB+", TRUE));
	g_ptr_array_add(sel, bar_meeting_source_new(
		"alsa_input.usb-audio-technica_AT2020USB_-00.analog-stereo",
		"AT2020USB+", FALSE));
	g_assert_true(bar_meeting_echo_risk(sel));

	/* Monitors alone cannot echo. */
	g_ptr_array_remove_index(sel, 1);
	g_assert_false(bar_meeting_echo_risk(sel));
}

/*
 * Templates are split before substitution.  Catches: a path with a
 * space becoming two arguments, a quote in a value breaking the line,
 * `; rm -rf ~' in a source name running as a command (there is no
 * shell), a value that itself contains `{output}' being expanded again.
 */
static void
test_template_tokenisation(void)
{
	const gchar *pairs[] = {
		"source", "mic; rm -rf ~ $(reboot)",
		"output", "/home/z/My \"Meetings\"/it's here.wav",
		"input", "{output}",
		NULL
	};
	g_auto(GStrv) argv = NULL;
	g_auto(GStrv) quoted = NULL;
	GError *error = NULL;

	argv = bar_meeting_expand_template(
		"pw-record --target {source} --opt=x{output}y {input} "
		"{unknown} -f", pairs, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(g_strv_length(argv), ==, 7);
	g_assert_cmpstr(argv[0], ==, "pw-record");
	g_assert_cmpstr(argv[2], ==, "mic; rm -rf ~ $(reboot)");
	g_assert_cmpstr(argv[3], ==,
	                "--opt=x/home/z/My \"Meetings\"/it's here.wavy");
	/* A value is not searched for placeholders. */
	g_assert_cmpstr(argv[4], ==, "{output}");
	g_assert_cmpstr(argv[5], ==, "{unknown}");

	/* The template's own quoting is honoured. */
	quoted = bar_meeting_expand_template(
		"'/opt/my tools/rec' \"{output}\"", pairs, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(g_strv_length(quoted), ==, 2);
	g_assert_cmpstr(quoted[0], ==, "/opt/my tools/rec");
	g_assert_cmpstr(quoted[1], ==, pairs[3]);

	g_assert_null(bar_meeting_expand_template("rec 'open", pairs,
	                                          &error));
	g_assert_nonnull(error);
	g_clear_error(&error);
	g_assert_null(bar_meeting_expand_template("", pairs, &error));
	g_assert_nonnull(error);
	g_clear_error(&error);
}

/*
 * The mix.  Catches: running ffmpeg for one input, amix's default
 * normalisation (which makes a quiet remote voice inaudible), and a
 * missing resample to the 16 kHz mono whisper wants.
 */
static void
test_mix_argv(void)
{
	const gchar *one[] = { "/s/a.wav", NULL };
	const gchar *three[] = { "/s/a.wav", "/s/b c.wav", "/s/d.wav", NULL };
	g_auto(GStrv) argv = NULL;
	g_autofree gchar *joined = NULL;
	guint n;

	g_assert_null(bar_meeting_mix_argv(NULL, one, "/s/m.wav"));

	argv = bar_meeting_mix_argv("/usr/bin/ffmpeg", three, "/s/m.wav");
	n = g_strv_length(argv);
	joined = g_strjoinv("|", argv);
	g_assert_cmpstr(argv[0], ==, "/usr/bin/ffmpeg");
	g_assert_cmpstr(argv[n - 1], ==, "/s/m.wav");
	g_assert_nonnull(strstr(joined, "|-i|/s/b c.wav|"));
	g_assert_nonnull(strstr(joined,
		"amix=inputs=3:duration=longest:normalize=0"));
	g_assert_nonnull(strstr(joined, "|-ar|16000|-ac|1|"));
	g_assert_nonnull(strstr(joined, "|-y|"));
}

/*
 * Names.  Catches: a title that walks out of the notes directory, a
 * slug longer than a file name should be, and the file-name shape the
 * notes repository expects.
 */
static void
test_names(void)
{
	g_autoptr(GDateTime) when = NULL;
	g_autofree gchar *c1 = bar_meeting_format_clock(65);
	g_autofree gchar *c2 = bar_meeting_format_clock(3725);
	g_autofree gchar *slug = bar_meeting_slug("../../etc/passwd; rm -rf ~");
	g_autofree gchar *uni = bar_meeting_slug("Réunion — Q4 plan!");
	g_autofree gchar *long_slug = bar_meeting_slug(
		"a very long meeting title that goes on and on and on forever");
	g_autofree gchar *session = NULL;
	g_autofree gchar *plain = NULL;
	g_autofree gchar *titled = NULL;
	g_autofree gchar *md = NULL;
	g_autofree gchar *rec = NULL;
	g_autofree gchar *title = NULL;

	when = g_date_time_new_local(2026, 10, 16, 14, 5, 9);
	session = bar_meeting_session_name(when);
	plain   = bar_meeting_notes_filename(when, NULL, "org");
	titled  = bar_meeting_notes_filename(when, "Q4 Planning", "org");
	md      = bar_meeting_notes_filename(when, "", "md");
	rec     = bar_meeting_recording_filename(0, "alsa_input.x/../y");
	title   = bar_meeting_default_title(when);

	g_assert_cmpstr(c1, ==, "01:05");
	g_assert_cmpstr(c2, ==, "1:02:05");
	g_assert_cmpstr(slug, ==, "etc_passwd_rm_rf");
	g_assert_cmpstr(uni, ==, "r_union_q4_plan");
	g_assert_cmpuint(strlen(long_slug), <=, 40);
	g_assert_false(g_str_has_suffix(long_slug, "_"));
	g_assert_cmpstr(session, ==, "2026-10-16_14-05-09");
	g_assert_cmpstr(plain, ==, "2026-10-16_14-05_meeting.org");
	g_assert_cmpstr(titled, ==, "2026-10-16_14-05_meeting_q4_planning.org");
	g_assert_cmpstr(md, ==, "2026-10-16_14-05_meeting.md");
	g_assert_cmpstr(rec, ==, "01-alsa_input_x____y.wav");
	g_assert_cmpstr(title, ==, "Meeting 2026-10-16 14:05");
}

/*
 * Splitting a summary.  Catches: action items lost or duplicated in
 * the Summary section, a `* Summary' heading repeated under the notes'
 * own, and Markdown and plain-label variants a model may produce.
 */
static void
test_summary_split(void)
{
	gchar *actions = NULL;
	g_autofree gchar *rest = NULL;
	g_autofree gchar *md_rest = NULL;
	g_autofree gchar *plain_rest = NULL;
	g_autofree gchar *none_rest = NULL;

	rest = bar_meeting_split_summary(
		"* Summary\nWe met.\n** Decisions\n- Ship\n"
		"** Action items\n- [ ] Zach: docs\n- [ ] Ana: tests\n"
		"** Open questions\n- When?\n", &actions);
	g_assert_cmpstr(actions, ==, "- [ ] Zach: docs\n- [ ] Ana: tests");
	g_assert_null(strstr(rest, "Zach"));
	g_assert_null(strstr(rest, "* Summary"));
	g_assert_nonnull(strstr(rest, "** Decisions\n- Ship"));
	g_assert_nonnull(strstr(rest, "** Open questions"));
	g_clear_pointer(&actions, g_free);

	md_rest = bar_meeting_split_summary(
		"## TL;DR\nShort.\n### Action Items:\n- [ ] Bo: call\n"
		"## Decisions\nNone.", &actions);
	g_assert_cmpstr(actions, ==, "- [ ] Bo: call");
	g_assert_nonnull(strstr(md_rest, "Short."));
	g_assert_nonnull(strstr(md_rest, "## Decisions"));
	g_clear_pointer(&actions, g_free);

	plain_rest = bar_meeting_split_summary(
		"Text.\nAction items:\n- [ ] X: y\n", &actions);
	g_assert_cmpstr(actions, ==, "- [ ] X: y");
	g_assert_cmpstr(plain_rest, ==, "Text.");
	g_clear_pointer(&actions, g_free);

	/* `**bold**' is not a heading. */
	none_rest = bar_meeting_split_summary("**Action items** are below",
	                                      &actions);
	g_assert_null(actions);
	g_assert_cmpstr(none_rest, ==, "**Action items** are below");
}

/*
 * The notes document, both formats.  Catches: a header missing one of
 * the repository's seven keys, an offset-less timestamp, a model's
 * `* Decisions' ending the Summary section early, a spoken line that
 * becomes an Org heading or keyword, and the section order.
 */
static void
test_document(void)
{
	g_autoptr(GTimeZone) tz = g_time_zone_new_identifier("-05:00");
	g_autoptr(GDateTime) when = g_date_time_new(tz, 2026, 10, 16, 14, 5,
	                                            0);
	BarMeetingDoc doc;
	g_autofree gchar *org = NULL;
	g_autofree gchar *md = NULL;
	g_autofree gchar *bare = NULL;
	const gchar *s, *a, *t, *n;

	memset(&doc, 0, sizeof(doc));
	doc.format = "org";
	doc.title = "Q4 \"Planning\"";
	doc.created = when;
	doc.author = "Zach Podbielniak";
	doc.summary = "* Summary\nWe met.\n* Decisions\n- Ship\n"
	              "* Action items\n- [ ] Zach: docs\n";
	doc.transcript = "Hello all.\n* starred speech\n#+title: hijack\n"
	                 "# hash\n";
	doc.duration_s = 125;
	doc.n_sources = 2;
	doc.recordings = "/rec/2026-10-16_14-05-00";
	org = bar_meeting_document(&doc);

	g_assert_true(g_str_has_prefix(org, "#+title: Q4 \"Planning\"\n"));
	g_assert_nonnull(strstr(org, "#+description: Meeting notes: 02:05 "
	                             "recorded from 2 sources\n"));
	g_assert_nonnull(strstr(org, "#+authors: Zach Podbielniak\n"));
	g_assert_nonnull(strstr(org, "#+categories: meeting\n"));
	g_assert_nonnull(strstr(org, "#+created: 2026-10-16T14:05:00-05:00\n"));
	g_assert_nonnull(strstr(org, "#+updated: 2026-10-16T14:05:00-05:00\n"));
	g_assert_nonnull(strstr(org, "#+version: 1.0\n"));

	s = strstr(org, "\n* Summary\n");
	a = strstr(org, "\n* Action items\n");
	t = strstr(org, "\n* Transcript\n");
	n = strstr(org, "\n* My notes\n");
	g_assert_true(s != NULL && a != NULL && t != NULL && n != NULL);
	g_assert_true(s < a && a < t && t < n);
	/* Re-levelled under Summary; the model's own `* Summary' gone. */
	g_assert_nonnull(strstr(org, "\n** Decisions\n- Ship\n"));
	g_assert_null(strstr(org, "* Summary\nWe met.\n* Summary"));
	g_assert_nonnull(strstr(a, "- [ ] Zach: docs"));
	/* Speech never becomes markup. */
	g_assert_nonnull(strstr(t, "\n * starred speech\n"));
	g_assert_nonnull(strstr(t, "\n #+title: hijack\n"));
	g_assert_null(strstr(t, "\n#+title: hijack"));
	g_assert_nonnull(strstr(t, "[[file:/rec/2026-10-16_14-05-00]]"));

	doc.format = "md";
	md = bar_meeting_document(&doc);
	g_assert_true(g_str_has_prefix(md, "---\ntitle: \"Q4 \\\"Planning\\\"\"\n"));
	g_assert_nonnull(strstr(md, "categories: [meeting]\n"));
	g_assert_nonnull(strstr(md, "created: 2026-10-16T14:05:00-05:00\n"));
	g_assert_nonnull(strstr(md, "\n## Summary\n"));
	g_assert_nonnull(strstr(md, "\n### Decisions\n"));
	g_assert_nonnull(strstr(md, "\n## Action items\n"));
	g_assert_nonnull(strstr(md, "\n\\# hash\n"));
	g_assert_nonnull(strstr(md, "\n## My notes\n"));

	/* No summary: the note says why, and nothing is invented. */
	doc.format = "org";
	doc.summary = NULL;
	doc.summary_note = "Summaries are turned off.";
	bare = bar_meeting_document(&doc);
	g_assert_nonnull(strstr(bare, "* Summary\nSummaries are turned off."));
	g_assert_nonnull(strstr(bare, "None captured --- there is no summary"));
}

/*
 * The emacsctl form.  Catches: a quote or backslash in a title or path
 * breaking out of its string literal, a newline in a title, and text
 * D-Bus would refuse being sent at all.
 */
static void
test_cmacs_form(void)
{
	const gchar *files[] = { "01-a.wav", "02-b \"c\".wav", NULL };
	const gchar *nofiles[] = { NULL };
	g_autofree gchar *form = NULL;
	g_autofree gchar *bare = NULL;

	form = bar_meeting_cmacs_form("meeting-notes", "mn-1",
		"/rec/x\\y", files, "Q4 \") (delete-file \"x\")", "secondbrain");
	g_assert_cmpstr(form, ==,
		"(cmacs-gowl-bar-meeting-finish \"meeting-notes\" \"mn-1\" "
		"\"/rec/x\\\\y\" (list \"01-a.wav\" \"02-b \\\"c\\\".wav\") "
		"\"Q4 \\\") (delete-file \\\"x\\\")\" \"secondbrain\")");

	bare = bar_meeting_cmacs_form("w", "i", "/d", files, "", NULL);
	g_assert_true(g_str_has_suffix(bare, ") nil nil)"));

	g_assert_null(bar_meeting_cmacs_form("w", "i", "/d", files,
	                                     "two\nlines", NULL));
	g_assert_null(bar_meeting_cmacs_form("w", "i", "/d\xff", files,
	                                     NULL, NULL));
	g_assert_null(bar_meeting_cmacs_form("w", "i", "/d", nofiles,
	                                     NULL, NULL));
}

/* Phases cmacs may push, and only those. */
static void
test_phase_names(void)
{
	BarMeetingPhase p;

	g_assert_true(bar_meeting_phase_from_name("summarizing", &p));
	g_assert_cmpint(p, ==, BAR_MEETING_SUMMARISING);
	g_assert_true(bar_meeting_phase_from_name("DONE", &p));
	g_assert_cmpint(p, ==, BAR_MEETING_DONE);
	/* A push may not claim the recorders are running. */
	g_assert_false(bar_meeting_phase_from_name("recording", &p));
	g_assert_false(bar_meeting_phase_from_name(NULL, &p));
	g_assert_true(bar_meeting_phase_is_busy(BAR_MEETING_FILING));
	g_assert_false(bar_meeting_phase_is_busy(BAR_MEETING_ERROR));
}

/* ══ The widget, against stand-in commands ══════════════════════════ */

typedef struct {
	BarHarness  h;
	gchar      *rec_dir;
	gchar      *out_dir;
	gchar      *argfile;   /* what the fake emacsctl / ffmpeg saw */
} Rig;

/*
 * A widget with every command pointed at a stand-in.  @recorder is the
 * body of the fake pw-record; NULL for the well-behaved default.
 */
static gboolean
rig_up(Rig *r, const gchar *recorder)
{
	g_autofree gchar *pactl = NULL;
	g_autofree gchar *rec = NULL;
	g_autofree gchar *ffmpeg = NULL;
	g_autofree gchar *whisper = NULL;
	g_autofree gchar *ai = NULL;
	g_autofree gchar *model = NULL;
	g_autofree gchar *body = NULL;
	g_autofree gchar *ffbody = NULL;

	memset(r, 0, sizeof(*r));
	if (!bar_harness_up(&r->h, "bar_register_meeting_plugins",
	                    "meeting-notes"))
		return FALSE;

	r->rec_dir = bar_harness_path(&r->h, "rec dir");
	r->out_dir = bar_harness_path(&r->h, "notes");
	r->argfile = bar_harness_path(&r->h, "argv.txt");

	body = g_strdup_printf(
		"case \"$1\" in\n"
		"  -f) cat '%s/pactl-sources.json' ;;\n"
		"  get-default-sink) echo '" SINK "' ;;\n"
		"  get-default-source) echo '" MIC "' ;;\n"
		"  *) exit 1 ;;\n"
		"esac", FIX_DIR);
	pactl = bar_harness_script(&r->h, "fake-pactl", body);

	/* Writes a header-plus-data WAV and its pid, then records until
	   SIGINT, the signal pw-record finishes a file on. */
	rec = bar_harness_script(&r->h, "fake-pw-record",
		(recorder != NULL) ? recorder :
		"out=\"${@: -1}\"\n"
		"echo $$ > \"$out.pid\"\n"
		"printf 'RIFF' > \"$out\"\n"
		"head -c 4000 /dev/zero >> \"$out\"\n"
		"trap 'echo stopped-by-int >> \"$out.pid\"; exit 0' INT\n"
		"while :; do sleep 0.02; done");

	ffbody = g_strdup_printf(
		"printf '%%s\\n' \"$@\" > '%s'\n"
		"printf 'RIFF' > \"${@: -1}\"\n"
		"head -c 2000 /dev/zero >> \"${@: -1}\"", r->argfile);
	ffmpeg = bar_harness_script(&r->h, "fake-ffmpeg", ffbody);

	whisper = bar_harness_script(&r->h, "fake-whisper-cli",
		"while [ $# -gt 0 ]; do\n"
		"  if [ \"$1\" = -of ]; then base=\"$2\"; fi\n"
		"  shift\n"
		"done\n"
		"printf 'Welcome everyone.\\n* not a heading\\n"
		"We will ship on Friday.\\n' > \"$base.txt\"");

	ai = bar_harness_script(&r->h, "fake-ai",
		"cat > /dev/null\n"
		"printf '* Summary\\nWe agreed to ship.\\n** Decisions\\n"
		"- Ship Friday\\n** Action items\\n- [ ] Zach: write docs\\n'");

	model = bar_harness_path(&r->h, "ggml-tiny.bin");
	g_assert_true(g_file_set_contents(model, "model", -1, NULL));

	gowl_bar_plugin_set_setting(r->h.plugin, "pactl", pactl);
	{
		g_autofree gchar *rc = g_strdup_printf(
			"'%s' --target {source} --rate 16000 "
			"--channels 1 --format s16 {output}", rec);

		gowl_bar_plugin_set_setting(r->h.plugin, "record-command",
		                            rc);
	}
	gowl_bar_plugin_set_setting(r->h.plugin, "ffmpeg", ffmpeg);
	{
		g_autofree gchar *tc = g_strdup_printf(
			"'%s' -m {model} -l {language} -t {threads} -otxt "
			"-of {output_base} {input}", whisper);

		gowl_bar_plugin_set_setting(r->h.plugin, "transcribe-command",
		                            tc);
	}
	gowl_bar_plugin_set_setting(r->h.plugin, "whisper-model", model);
	gowl_bar_plugin_set_setting(r->h.plugin, "ai-backend", "ai");
	gowl_bar_plugin_set_setting(r->h.plugin, "ai-command", ai);
	gowl_bar_plugin_set_setting(r->h.plugin, "recordings-dir",
	                            r->rec_dir);
	gowl_bar_plugin_set_setting(r->h.plugin, "output-dir", r->out_dir);
	gowl_bar_plugin_set_setting(r->h.plugin, "whisper-models-dir",
	                            r->h.tmp);
	gowl_bar_plugin_set_setting(r->h.plugin, "open-command",
	                            "fake-open");
	bar_harness_activate(&r->h);
	bar_harness_set(&r->h, "backend", "local");
	/* The first poll reads the sources and the defaults. */
	bar_harness_poll(&r->h);
	return TRUE;
}

static void
rig_down(Rig *r)
{
	bar_harness_down(&r->h);
	g_free(r->rec_dir);
	g_free(r->out_dir);
	g_free(r->argfile);
}

static gboolean
label_is(Rig *r, const gchar *prefix)
{
	g_autofree gchar *label = gowl_bar_plugin_dup_label(r->h.plugin);

	if (prefix == NULL)
		return label == NULL;
	return label != NULL && g_str_has_prefix(label, prefix);
}

/* Poll until the toast list mentions @needle, or ~5 s pass. */
static gboolean
pump_until_toast(Rig *r, const gchar *needle)
{
	gint i;

	for (i = 0; i < 100; i++) {
		bar_harness_poll(&r->h);
		if (bar_harness_toasted(&r->h, needle))
			return TRUE;
		g_usleep(50 * 1000);
	}
	return FALSE;
}

static void
click(Rig *r, guint button)
{
	g_assert_true(gowl_bar_plugin_on_click(r->h.plugin, button, 0, 0, 0));
}

/* Every `*.pid' file in the session: each recorder must have been
   reaped (ESRCH; a zombie would still answer kill 0). */
static guint
assert_recorders_reaped(Rig *r, gboolean expect_int)
{
	GDir *top, *sess;
	const gchar *sname, *fname;
	guint n = 0;

	top = g_dir_open(r->rec_dir, 0, NULL);
	g_assert_nonnull(top);
	while ((sname = g_dir_read_name(top)) != NULL) {
		g_autofree gchar *sdir = g_build_filename(r->rec_dir, sname,
		                                          NULL);

		sess = g_dir_open(sdir, 0, NULL);
		g_assert_nonnull(sess);
		while ((fname = g_dir_read_name(sess)) != NULL) {
			g_autofree gchar *path = NULL;
			g_autofree gchar *text = NULL;
			pid_t pid;

			if (!g_str_has_suffix(fname, ".pid"))
				continue;
			path = g_build_filename(sdir, fname, NULL);
			g_assert_true(g_file_get_contents(path, &text, NULL,
			                                  NULL));
			pid = (pid_t)atoi(text);
			/* Killed mid-write by an immediate cancel: nothing to
			   check, and only acceptable when not expecting INT. */
			if (pid <= 0 && !expect_int)
				continue;
			g_assert_cmpint(pid, >, 0);
			g_assert_cmpint(kill(pid, 0), ==, -1);
			g_assert_cmpint(errno, ==, ESRCH);
			if (expect_int)
				g_assert_nonnull(strstr(text, "stopped-by-int"));
			n++;
		}
		g_dir_close(sess);
	}
	g_dir_close(top);
	return n;
}

static gchar *
only_file_in(const gchar *dir)
{
	GDir *d = g_dir_open(dir, 0, NULL);
	const gchar *name;
	gchar *found = NULL;
	guint n = 0;

	g_assert_nonnull(d);
	while ((name = g_dir_read_name(d)) != NULL) {
		g_free(found);
		found = g_build_filename(dir, name, NULL);
		n++;
	}
	g_dir_close(d);
	g_assert_cmpuint(n, ==, 1);
	return found;
}

/*
 * Idle: an icon, no label, the idle colour, and each colour setting
 * honoured as a role or a #rrggbb literal.  Catches: a widget stuck in
 * a role the user overrode, and a hex literal silently dropped.
 */
static void
test_widget_idle_and_colours(void)
{
	Rig r;
	gdouble rgba[4];
	g_autofree gchar *icon = NULL;

	if (!rig_up(&r, NULL))
		return;

	icon = gowl_bar_plugin_dup_icon(r.h.plugin);
	g_assert_nonnull(icon);
	g_assert_true(label_is(&r, NULL));
	g_assert_cmpint(gowl_bar_plugin_get_color(r.h.plugin), ==,
	                GOWL_BAR_COLOR_SUBTEXT);

	bar_harness_set(&r.h, "color-idle", "#102030");
	g_assert_true(gowl_bar_plugin_get_color_rgba(r.h.plugin, rgba));
	g_assert_cmpfloat_with_epsilon(rgba[0], 0x10 / 255.0, 1e-6);

	bar_harness_set(&r.h, "color-idle", "peach");
	g_assert_false(gowl_bar_plugin_get_color_rgba(r.h.plugin, rgba));
	g_assert_cmpint(gowl_bar_plugin_get_color(r.h.plugin), ==,
	                GOWL_BAR_COLOR_PEACH);

	rig_down(&r);
}

/*
 * The whole local road, with real child processes: two recorders
 * (the default sink's monitor and the default microphone), SIGINT on
 * stop, both reaped, the mix, whisper, the summary from `ai', and an
 * Org file with the right name, header and sections.  Also: right-click
 * opens the panel without starting anything, and the colours follow the
 * state.
 */
static void
test_widget_local_end_to_end(void)
{
	Rig r;
	g_autofree gchar *path = NULL;
	g_autofree gchar *text = NULL;
	g_autofree gchar *mixargs = NULL;
	g_autofree gchar *base = NULL;
	g_autofree gchar *tooltip = NULL;
	gint i;

	if (!rig_up(&r, NULL))
		return;

	/* Right-click: the panel, not a recording. */
	click(&r, BTN_RIGHT);
	g_assert_cmpuint(r.h.host->opened, ==, 1);
	g_assert_true(label_is(&r, NULL));

	bar_harness_set(&r.h, "color-recording", "peach");
	gowl_bar_plugin_panel_text(r.h.plugin, "title", "Q4 Planning", FALSE);
	click(&r, BTN_LEFT);
	g_assert_true(label_is(&r, "\xe2\x97\x8f 00:0"));
	g_assert_cmpint(gowl_bar_plugin_get_color(r.h.plugin), ==,
	                GOWL_BAR_COLOR_PEACH);

	/* Let the recorders write, then stop. */
	g_usleep(200 * 1000);
	bar_harness_poll(&r.h);
	g_assert_true(label_is(&r, "\xe2\x97\x8f"));
	click(&r, BTN_LEFT);
	g_assert_true(label_is(&r, "stopping"));
	g_assert_cmpint(gowl_bar_plugin_get_color(r.h.plugin), ==,
	                GOWL_BAR_COLOR_PEACH);

	g_assert_true(pump_until_toast(&r, "Meeting notes saved"));
	g_assert_cmpuint(assert_recorders_reaped(&r, TRUE), ==, 2);
	g_assert_true(label_is(&r, NULL));
	g_assert_cmpint(gowl_bar_plugin_get_color(r.h.plugin), ==,
	                GOWL_BAR_COLOR_SUBTEXT);

	/* Two sources were mixed, without normalisation. */
	g_assert_true(g_file_get_contents(r.argfile, &mixargs, NULL, NULL));
	g_assert_nonnull(strstr(mixargs, "amix=inputs=2:"));
	g_assert_nonnull(strstr(mixargs,
		"/01-alsa_output_usb-Schiit_Audio_Schiit_Modi_3E-00_"));
	g_assert_nonnull(strstr(mixargs, "/02-alsa_input_pci-0000_c1_00_6_"));

	path = only_file_in(r.out_dir);
	base = g_path_get_basename(path);
	g_assert_true(g_str_has_suffix(base, "_meeting_q4_planning.org"));
	g_assert_true(g_file_get_contents(path, &text, NULL, NULL));
	g_assert_true(g_str_has_prefix(text, "#+title: Q4 Planning\n"));
	g_assert_nonnull(strstr(text, "#+categories: meeting\n"));
	g_assert_nonnull(strstr(text, "* Summary\nWe agreed to ship.\n"));
	g_assert_nonnull(strstr(text, "** Decisions\n- Ship Friday\n"));
	g_assert_nonnull(strstr(text,
		"* Action items\n- [ ] Zach: write docs\n"));
	g_assert_nonnull(strstr(text, "* Transcript\nWelcome everyone.\n"
	                              " * not a heading\n"));
	g_assert_nonnull(strstr(text, "\n* My notes\n"));

	tooltip = gowl_bar_plugin_dup_tooltip(r.h.plugin);
	g_assert_nonnull(strstr(tooltip, base));

	/* Recent lists it, and a click opens it with the open command. */
	gowl_bar_plugin_panel_opened(r.h.plugin);
	{
		g_autoptr(GowlBarPanel) panel = bar_harness_panel(&r.h);
		g_autofree gchar *id = g_strdup_printf("open:%s", path);

		g_assert_true(bar_harness_panel_mentions(panel, base));
		gowl_bar_plugin_panel_action(r.h.plugin, id, 0, 0, 1);
		g_assert_true(bar_harness_spawned(&r.h, "fake-open '"));
	}
	gowl_bar_plugin_panel_closed(r.h.plugin);

	/* Nothing still running: one more poll reaps nothing new. */
	for (i = 0; i < 3; i++)
		bar_harness_poll(&r.h);
	g_assert_true(label_is(&r, NULL));
	rig_down(&r);
}

/*
 * Markdown, with summaries off.  Catches: the format setting ignored,
 * and a transcript lost because there was nothing to summarise with.
 */
static void
test_widget_markdown_without_summary(void)
{
	Rig r;
	g_autofree gchar *path = NULL;
	g_autofree gchar *text = NULL;

	if (!rig_up(&r, NULL))
		return;
	bar_harness_set(&r.h, "format", "md");
	bar_harness_set(&r.h, "summary", "false");

	click(&r, BTN_LEFT);
	g_usleep(100 * 1000);
	click(&r, BTN_LEFT);
	g_assert_true(pump_until_toast(&r, "saved (no summary)"));

	path = only_file_in(r.out_dir);
	g_assert_true(g_str_has_suffix(path, "_meeting.md"));
	g_assert_true(g_file_get_contents(path, &text, NULL, NULL));
	g_assert_true(g_str_has_prefix(text, "---\ntitle: \"Meeting "));
	g_assert_nonnull(strstr(text, "## Summary\n\nSummaries are turned off"));
	g_assert_nonnull(strstr(text, "## Transcript\n\nWelcome everyone."));
	rig_down(&r);
}

/*
 * One recorder dies at once.  Catches: a silent early death (the user
 * thinks the microphone is being recorded), and the other recorder
 * being taken down with it.
 */
static void
test_widget_early_death(void)
{
	Rig r;

	if (!rig_up(&r,
		"out=\"${@: -1}\"\n"
		"case \"$out\" in\n"
		"  *Mic1*) echo 'target node not found: Mic1' >&2; exit 1 ;;\n"
		"esac\n"
		"echo $$ > \"$out.pid\"\n"
		"printf 'RIFF' > \"$out\"; head -c 4000 /dev/zero >> \"$out\"\n"
		"trap 'echo stopped-by-int >> \"$out.pid\"; exit 0' INT\n"
		"while :; do sleep 0.02; done"))
		return;

	click(&r, BTN_LEFT);
	g_assert_true(pump_until_toast(&r, "A recorder stopped early"));
	g_assert_true(bar_harness_toasted(&r.h,
	                                  "target node not found: Mic1"));
	/* Still recording the other one. */
	g_assert_true(label_is(&r, "\xe2\x97\x8f"));

	click(&r, BTN_LEFT);
	g_assert_true(pump_until_toast(&r, "Meeting notes saved"));
	g_assert_cmpuint(assert_recorders_reaped(&r, TRUE), ==, 1);
	rig_down(&r);
}

/*
 * The duration limit.  Catches: a forgotten recording filling the disk
 * all weekend; the stop must say why.
 */
static void
test_widget_max_duration(void)
{
	Rig r;

	if (!rig_up(&r, NULL))
		return;
	bar_harness_set(&r.h, "max-minutes", "0.005");   /* 0.3 s */

	click(&r, BTN_LEFT);
	g_usleep(400 * 1000);
	g_assert_true(pump_until_toast(&r, "Meeting recording stopped"));
	g_assert_true(bar_harness_toasted(&r.h, "max-minutes"));
	g_assert_true(pump_until_toast(&r, "Meeting notes saved"));
	g_assert_cmpuint(assert_recorders_reaped(&r, TRUE), ==, 2);
	rig_down(&r);
}

/*
 * A recorder that ignores SIGINT.  Catches: a stop that waits forever
 * --- SIGTERM must follow after the grace period, and the child must
 * still be reaped.
 */
static void
test_widget_sigint_ignored(void)
{
	Rig r;

	if (!rig_up(&r,
		"out=\"${@: -1}\"\n"
		"echo $$ > \"$out.pid\"\n"
		"printf 'RIFF' > \"$out\"; head -c 4000 /dev/zero >> \"$out\"\n"
		"trap '' INT\n"
		"trap 'exit 0' TERM\n"
		"while :; do sleep 0.02; done"))
		return;
	bar_harness_set(&r.h, "stop-grace", "0");

	click(&r, BTN_LEFT);
	g_usleep(100 * 1000);
	click(&r, BTN_LEFT);
	g_assert_true(pump_until_toast(&r, "Meeting notes saved"));
	g_assert_cmpuint(assert_recorders_reaped(&r, FALSE), ==, 2);
	rig_down(&r);
}

/*
 * No recorder can be started.  Catches: a spinning `starting' label,
 * and an error that does not name the package to install.
 */
static void
test_widget_missing_recorder_and_pactl(void)
{
	Rig r;
	g_autoptr(GowlBarPanel) panel = NULL;

	if (!rig_up(&r, NULL))
		return;
	bar_harness_set(&r.h, "record-command",
	                "/nonexistent/pw-record {source} {output}");
	click(&r, BTN_LEFT);
	bar_harness_poll(&r.h);
	g_assert_true(label_is(&r, "error"));
	g_assert_cmpint(gowl_bar_plugin_get_color(r.h.plugin), ==,
	                GOWL_BAR_COLOR_RED);
	g_assert_true(bar_harness_toasted(&r.h, "pipewire-utils"));

	/* pactl missing: the panel names the package. */
	bar_harness_set(&r.h, "pactl", "/nonexistent/pactl");
	gowl_bar_plugin_panel_action(r.h.plugin, "sources-refresh", 0, 0, 1);
	panel = bar_harness_panel(&r.h);
	g_assert_true(bar_harness_panel_mentions(panel, "pulseaudio-utils"));
	rig_down(&r);
}

/*
 * No whisper model configured.  Catches: the recordings thrown away,
 * and an error that does not say which setting to set.
 */
static void
test_widget_no_model(void)
{
	Rig r;
	g_autofree gchar *tooltip = NULL;

	if (!rig_up(&r, NULL))
		return;
	bar_harness_set(&r.h, "whisper-model", "/nonexistent/model.bin");
	click(&r, BTN_LEFT);
	g_usleep(100 * 1000);
	click(&r, BTN_LEFT);
	g_assert_true(pump_until_toast(&r, "Meeting notes failed"));
	tooltip = gowl_bar_plugin_dup_tooltip(r.h.plugin);
	g_assert_nonnull(strstr(tooltip, "whisper-model"));
	g_assert_nonnull(strstr(tooltip, r.rec_dir));
	g_assert_cmpuint(assert_recorders_reaped(&r, TRUE), ==, 2);
	rig_down(&r);
}

/*
 * The cmacs road: the form sent through emacsctl, then pushes.
 * Catches: a push for ANOTHER job taken as ours, the same push toasted
 * twice on a reconfigure, and the destination not reaching cmacs.
 */
static void
test_widget_cmacs_route(void)
{
	Rig r;
	g_autofree gchar *ctl = NULL;
	g_autofree gchar *body = NULL;
	g_autofree gchar *form = NULL;
	g_autofree gchar *id = NULL;
	const gchar *p, *q;
	guint toasts;
	gint i;

	if (!rig_up(&r, NULL))
		return;
	body = g_strdup_printf("[ \"$1\" = eval ] || exit 9\n"
	                       "printf '%%s' \"$2\" > '%s'\necho t",
	                       r.argfile);
	ctl = bar_harness_script(&r.h, "fake-emacsctl", body);
	bar_harness_set(&r.h, "emacsctl", ctl);
	bar_harness_set(&r.h, "backend", "cmacs");
	bar_harness_set(&r.h, "destination", "both");

	click(&r, BTN_LEFT);
	g_usleep(100 * 1000);
	click(&r, BTN_LEFT);
	for (i = 0; i < 100 && !g_file_test(r.argfile, G_FILE_TEST_EXISTS);
	     i++) {
		bar_harness_poll(&r.h);
		g_usleep(50 * 1000);
	}
	g_assert_true(g_file_get_contents(r.argfile, &form, NULL, NULL));
	g_assert_true(g_str_has_prefix(form,
		"(cmacs-gowl-bar-meeting-finish \"meeting-notes\" \"mn-"));
	g_assert_nonnull(strstr(form, "(list \"01-"));
	g_assert_nonnull(strstr(form, ".wav\" \"02-alsa_input_"));
	g_assert_true(g_str_has_suffix(form, " nil \"both\")"));
	g_assert_nonnull(strstr(form, r.rec_dir));
	g_assert_cmpuint(assert_recorders_reaped(&r, TRUE), ==, 2);

	p = strstr(form, "\"mn-");
	q = strchr(p + 1, '"');
	id = g_strndup(p + 1, (gsize)(q - p - 1));

	/* Someone else's job: ignored. */
	gowl_bar_plugin_set_setting(r.h.plugin, "job-id", "mn-0-deadbeef");
	gowl_bar_plugin_set_setting(r.h.plugin, "state", "error");
	bar_harness_set(&r.h, "error", "not yours");
	g_assert_false(label_is(&r, "error"));

	gowl_bar_plugin_set_setting(r.h.plugin, "job-id", id);
	gowl_bar_plugin_set_setting(r.h.plugin, "error", NULL);
	bar_harness_set(&r.h, "state", "transcribing");
	g_assert_true(label_is(&r, "transcribing"));
	g_assert_cmpint(gowl_bar_plugin_get_color(r.h.plugin), ==,
	                GOWL_BAR_COLOR_YELLOW);

	gowl_bar_plugin_set_setting(r.h.plugin, "result",
	                            "/notes/00_inbox/x.org");
	bar_harness_set(&r.h, "state", "done");
	g_assert_true(label_is(&r, NULL));
	g_assert_true(bar_harness_toasted(&r.h,
		"Meeting notes filed|/notes/00_inbox/x.org"));
	toasts = r.h.host->toasts->len;

	/* The same push again (every reconfigure re-delivers): nothing. */
	bar_harness_set(&r.h, "color-idle", "subtext");
	bar_harness_poll(&r.h);
	g_assert_cmpuint(r.h.host->toasts->len, ==, toasts);
	rig_down(&r);
}

/*
 * The panel: sources as toggles with the defaults on, the selection
 * saved to the state file, destinations under cmacs, models under the
 * local backend.  Catches: a toggle that changes nothing on disk (the
 * choice lost at the next login), and the echo warning never raised for
 * the speakers-into-microphone case.
 */
static void
test_widget_panel(void)
{
	Rig r;
	g_autofree gchar *state = NULL;
	g_autofree gchar *saved = NULL;
	g_autofree gchar *mon_id = g_strdup("src:" SINK ".monitor");

	if (!rig_up(&r, NULL))
		return;

	/* Opening reads the model list and recent notes on a worker. */
	gowl_bar_plugin_panel_opened(r.h.plugin);
	{
		g_autoptr(GowlBarPanel) panel = bar_harness_panel(&r.h);
		GowlBarPanelItem *item;

		g_assert_true(bar_harness_panel_mentions(panel,
			"Monitor of Schiit Modi 3E"));
		g_assert_true(bar_harness_panel_mentions(panel,
			"Start recording"));
		g_assert_true(bar_harness_panel_mentions(panel, "ggml-tiny.bin"));
		g_assert_true(bar_harness_panel_mentions(panel, "local"));
		item = gowl_bar_panel_find(panel, mon_id);
		g_assert_nonnull(item);
		g_assert_true(gowl_bar_panel_item_get_active(item));
		item = gowl_bar_panel_find(panel, "src:" SPEAKER_MON);
		g_assert_nonnull(item);
		g_assert_false(gowl_bar_panel_item_get_active(item));
	}

	/* Turn the Schiit monitor off and the laptop speakers on. */
	gowl_bar_plugin_panel_action(r.h.plugin, mon_id, 0, 0, 1);
	gowl_bar_plugin_panel_action(r.h.plugin, "src:" SPEAKER_MON, 0, 0, 1);
	state = g_build_filename(r.h.host->state_dir,
	                         "meeting-notes.sources", NULL);
	g_assert_true(g_file_get_contents(state, &saved, NULL, NULL));
	g_assert_null(strstr(saved, SINK ".monitor"));
	g_assert_nonnull(strstr(saved, SPEAKER_MON "\n"));
	g_assert_nonnull(strstr(saved, MIC "\n"));

	/* Cancelled recorders that swallow SIGINT get SIGTERM at once. */
	bar_harness_set(&r.h, "stop-grace", "0");
	/* Speakers + their microphone: warned, once. */
	gowl_bar_plugin_on_click(r.h.plugin, BTN_LEFT, 0, 0, 0);
	g_assert_true(bar_harness_toasted(&r.h,
	                                  "Speech may be recorded twice"));
	gowl_bar_plugin_panel_action(r.h.plugin, "control", 1, 0, 1);
	g_assert_true(bar_harness_toasted(&r.h, "Meeting cancelled"));
	/* The cancelled recorders are still collected: the widget polls
	   fast until its orphan list is empty. */
	{
		gint i;

		for (i = 0; i < 100 &&
		     gowl_bar_plugin_get_interval(r.h.plugin) != 30; i++) {
			bar_harness_poll(&r.h);
			g_usleep(20 * 1000);
		}
		g_assert_cmpint(gowl_bar_plugin_get_interval(r.h.plugin), ==,
		                30);
		/* SIGINT may land before a stand-in has even written its
		   pid, so count nothing --- but every one that did is gone. */
		assert_recorders_reaped(&r, FALSE);
	}

	/* A model row selects that model. */
	{
		g_autofree gchar *model = bar_harness_path(&r.h,
		                                           "ggml-tiny.bin");
		g_autofree gchar *id = g_strdup_printf("model:%s", model);

		bar_harness_set(&r.h, "whisper-model", "");
		gowl_bar_plugin_panel_action(r.h.plugin, id, 0, 0, 1);
		g_assert_cmpstr(gowl_bar_plugin_get_setting(r.h.plugin,
			"whisper-model"), ==, model);
	}

	/* Under cmacs: the four destinations, and a click picks one. */
	bar_harness_set(&r.h, "backend", "cmacs");
	{
		g_autoptr(GowlBarPanel) panel = bar_harness_panel(&r.h);

		g_assert_true(bar_harness_panel_mentions(panel, "Second brain"));
		g_assert_true(bar_harness_panel_mentions(panel, "Org inbox"));
		g_assert_true(bar_harness_panel_mentions(panel, "Brain + dir"));
	}
	gowl_bar_plugin_panel_action(r.h.plugin, "dest", 1, 0, 1);
	g_assert_cmpstr(gowl_bar_plugin_get_setting(r.h.plugin,
	                "destination"), ==, "secondbrain");

	gowl_bar_plugin_panel_closed(r.h.plugin);
	rig_down(&r);
}

int
main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/meeting-notes/sources/real-pactl",
	                test_sources_from_real_pactl);
	g_test_add_func("/meeting-notes/sources/hostile", test_sources_hostile);
	g_test_add_func("/meeting-notes/sources/selection", test_selection);
	g_test_add_func("/meeting-notes/sources/echo-risk", test_echo_risk);
	g_test_add_func("/meeting-notes/template/tokenisation",
	                test_template_tokenisation);
	g_test_add_func("/meeting-notes/template/mix", test_mix_argv);
	g_test_add_func("/meeting-notes/names", test_names);
	g_test_add_func("/meeting-notes/summary/split", test_summary_split);
	g_test_add_func("/meeting-notes/document", test_document);
	g_test_add_func("/meeting-notes/cmacs/form", test_cmacs_form);
	g_test_add_func("/meeting-notes/cmacs/phases", test_phase_names);
	g_test_add_func("/meeting-notes/widget/idle-and-colours",
	                test_widget_idle_and_colours);
	g_test_add_func("/meeting-notes/widget/local-end-to-end",
	                test_widget_local_end_to_end);
	g_test_add_func("/meeting-notes/widget/markdown-no-summary",
	                test_widget_markdown_without_summary);
	g_test_add_func("/meeting-notes/widget/early-death",
	                test_widget_early_death);
	g_test_add_func("/meeting-notes/widget/max-duration",
	                test_widget_max_duration);
	g_test_add_func("/meeting-notes/widget/sigint-ignored",
	                test_widget_sigint_ignored);
	g_test_add_func("/meeting-notes/widget/missing-deps",
	                test_widget_missing_recorder_and_pactl);
	g_test_add_func("/meeting-notes/widget/no-model", test_widget_no_model);
	g_test_add_func("/meeting-notes/widget/cmacs-route",
	                test_widget_cmacs_route);
	g_test_add_func("/meeting-notes/widget/panel", test_widget_panel);

	return g_test_run();
}
