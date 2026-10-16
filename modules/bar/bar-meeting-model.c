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
 * The pure half of the `meeting-notes' widget; see bar-meeting-model.h.
 *
 * Nothing in this file spawns, reads a file or touches the bar: every
 * function takes text and returns text, which is what lets the test
 * include this file directly and hand it the hostile cases.
 */

#include "bar-meeting-model.h"

#include <string.h>

#include "barkit/gowl-bar-json.h"
#include "bar-util.h"

/* ----------------------------------------------------------------
 * Phases
 * ---------------------------------------------------------------- */

static const gchar *const phase_names[] = {
	"idle", "starting", "recording", "stopping", "mixing",
	"transcribing", "summarising", "filing", "done", "error"
};

const gchar *
bar_meeting_phase_name(BarMeetingPhase phase)
{
	if ((guint)phase >= G_N_ELEMENTS(phase_names))
		return "idle";
	return phase_names[phase];
}

gboolean
bar_meeting_phase_from_name(const gchar *name, BarMeetingPhase *phase)
{
	/* Only the states cmacs pushes.  `recording' and the rest are
	   ours alone: a push claiming the recorders are running would be
	   a lie the widget could act on. */
	static const struct {
		const gchar     *name;
		BarMeetingPhase  phase;
	} table[] = {
		{ "mixing",       BAR_MEETING_MIXING },
		{ "transcribing", BAR_MEETING_TRANSCRIBING },
		{ "summarising",  BAR_MEETING_SUMMARISING },
		{ "summarizing",  BAR_MEETING_SUMMARISING },
		{ "filing",       BAR_MEETING_FILING },
		{ "done",         BAR_MEETING_DONE },
		{ "error",        BAR_MEETING_ERROR }
	};
	guint i;

	if (name == NULL)
		return FALSE;
	for (i = 0; i < G_N_ELEMENTS(table); i++) {
		if (g_ascii_strcasecmp(name, table[i].name) == 0) {
			if (phase != NULL)
				*phase = table[i].phase;
			return TRUE;
		}
	}
	return FALSE;
}

gboolean
bar_meeting_phase_is_busy(BarMeetingPhase phase)
{
	return phase != BAR_MEETING_IDLE &&
	       phase != BAR_MEETING_DONE &&
	       phase != BAR_MEETING_ERROR;
}

/* ----------------------------------------------------------------
 * Sources
 * ---------------------------------------------------------------- */

BarMeetingSource *
bar_meeting_source_new(const gchar *name, const gchar *description,
                       gboolean monitor)
{
	BarMeetingSource *src;

	src = g_new0(BarMeetingSource, 1);
	src->name        = g_strdup(name);
	src->description = g_strdup((description != NULL &&
	                             description[0] != '\0')
	                            ? description : name);
	src->monitor     = monitor;
	return src;
}

void
bar_meeting_source_free(BarMeetingSource *source)
{
	if (source == NULL)
		return;
	g_free(source->name);
	g_free(source->description);
	g_free(source);
}

gboolean
bar_meeting_name_is_valid(const gchar *name)
{
	const gchar *p;
	gsize len;

	if (name == NULL || name[0] == '\0' || name[0] == '-')
		return FALSE;
	len = strlen(name);
	if (len > BAR_MEETING_MAX_NAME)
		return FALSE;
	if (!g_utf8_validate(name, (gssize)len, NULL))
		return FALSE;
	for (p = name; *p != '\0'; p++)
		if ((guchar)*p < 0x20 || (guchar)*p == 0x7f)
			return FALSE;
	return TRUE;
}

/*
 * Skip a JSON string starting at @p (which points at the opening
 * quote).  Returns the character after the closing quote, or the NUL
 * when the string never closes.
 */
static const gchar *
skip_string(const gchar *p)
{
	p++;
	while (*p != '\0') {
		if (*p == '\\') {
			if (p[1] == '\0')
				return p + 1;
			p += 2;
			continue;
		}
		if (*p == '"')
			return p + 1;
		p++;
	}
	return p;
}

/*
 * The end of the object or array starting at @p (an opening brace or
 * bracket): the character after its match, or NULL when it never
 * closes.  Strings are skipped wholesale, so a brace inside a device
 * description cannot unbalance the count.
 */
static const gchar *
skip_container(const gchar *p)
{
	gint depth = 0;

	while (*p != '\0') {
		if (*p == '"') {
			p = skip_string(p);
			continue;
		}
		if (*p == '{' || *p == '[') {
			depth++;
		} else if (*p == '}' || *p == ']') {
			depth--;
			if (depth == 0)
				return p + 1;
		}
		p++;
	}
	return NULL;
}

/*
 * A copy of the object [@start, @end) with every NESTED object and
 * array replaced by `null'.  Reading `name' from that cannot land on a
 * port's name, whatever order pactl prints the fields in.
 */
static gchar *
shallow_object(const gchar *start, const gchar *end)
{
	GString *out;
	const gchar *p;

	out = g_string_sized_new((gsize)(end - start));
	g_string_append_c(out, '{');
	p = start + 1;
	while (p < end - 1) {
		if (*p == '"') {
			const gchar *q = skip_string(p);

			if (q > end)
				q = end;
			g_string_append_len(out, p, (gssize)(q - p));
			p = q;
			continue;
		}
		if (*p == '{' || *p == '[') {
			const gchar *q = skip_container(p);

			if (q == NULL || q > end)
				break;
			g_string_append(out, "null");
			p = q;
			continue;
		}
		g_string_append_c(out, *p);
		p++;
	}
	g_string_append_c(out, '}');
	return g_string_free(out, FALSE);
}

static gint
source_order(gconstpointer a, gconstpointer b)
{
	const BarMeetingSource *sa = *(BarMeetingSource * const *)a;
	const BarMeetingSource *sb = *(BarMeetingSource * const *)b;

	/* Monitors first: "what the meeting sounds like" is the source
	   people look for first; then the order pactl gave, which the
	   stable sort keeps. */
	if (sa->monitor != sb->monitor)
		return sa->monitor ? -1 : 1;
	return 0;
}

GPtrArray *
bar_meeting_parse_sources(const gchar *json)
{
	GPtrArray *out;
	const gchar *p;

	out = g_ptr_array_new_with_free_func(
		(GDestroyNotify)bar_meeting_source_free);
	if (json == NULL)
		return out;

	/* The listing is a top-level array of objects; walk it object by
	   object rather than searching, so one malformed member ends the
	   walk instead of shifting every later field by one source. */
	p = strchr(json, '[');
	if (p == NULL)
		return out;
	p++;

	while (*p != '\0' && out->len < BAR_MEETING_MAX_SOURCES) {
		const gchar *end;
		g_autofree gchar *shallow = NULL;
		g_autofree gchar *full = NULL;
		g_autofree gchar *name = NULL;
		g_autofree gchar *desc = NULL;
		g_autofree gchar *props = NULL;
		g_autofree gchar *klass = NULL;
		g_autofree gchar *clean_desc = NULL;
		gboolean monitor;

		while (*p == ' ' || *p == ',' || *p == '\n' || *p == '\r' ||
		       *p == '\t')
			p++;
		if (*p != '{')
			break;
		end = skip_container(p);
		if (end == NULL)
			break;

		full    = g_strndup(p, (gsize)(end - p));
		shallow = shallow_object(p, end);
		p = end;

		name = gowl_bar_json_string(shallow, "name");
		if (!bar_meeting_name_is_valid(name))
			continue;
		desc  = gowl_bar_json_string(shallow, "description");
		props = gowl_bar_json_object(full, "properties");
		klass = (props != NULL)
			? gowl_bar_json_string(props, "device.class") : NULL;

		monitor = g_str_has_suffix(name, ".monitor") ||
		          g_strcmp0(klass, "monitor") == 0;
		clean_desc = bar_meeting_clean_text(desc, 200);
		g_ptr_array_add(out, bar_meeting_source_new(name, clean_desc,
		                                            monitor));
	}

	/* g_ptr_array_sort is not stable; insert-sort keeps pactl's order
	   within each group, and the list is tiny. */
	{
		guint i, j;

		for (i = 1; i < out->len; i++) {
			gpointer cur = g_ptr_array_index(out, i);

			j = i;
			while (j > 0 && source_order(
				       &g_ptr_array_index(out, j - 1),
				       &cur) > 0) {
				out->pdata[j] = out->pdata[j - 1];
				j--;
			}
			out->pdata[j] = cur;
		}
	}
	return out;
}

gchar **
bar_meeting_default_selection(const gchar *default_sink,
                              const gchar *default_source)
{
	GPtrArray *names;

	names = g_ptr_array_new();
	if (default_sink != NULL && default_sink[0] != '\0') {
		gchar *mon = g_strdup_printf("%s.monitor", default_sink);

		if (bar_meeting_name_is_valid(mon))
			g_ptr_array_add(names, mon);
		else
			g_free(mon);
	}
	if (bar_meeting_name_is_valid(default_source)) {
		gboolean dup = (names->len > 0 &&
		                g_strcmp0(g_ptr_array_index(names, 0),
		                          default_source) == 0);

		/* With nothing plugged in, the default source can BE the
		   sink's monitor; recording it twice is the same audio
		   mixed with itself. */
		if (!dup)
			g_ptr_array_add(names, g_strdup(default_source));
	}
	g_ptr_array_add(names, NULL);
	return (gchar **)g_ptr_array_free(names, FALSE);
}

gchar **
bar_meeting_parse_selection(const gchar *text)
{
	GPtrArray *names;
	g_auto(GStrv) lines = NULL;
	guint i;

	names = g_ptr_array_new();
	if (text != NULL) {
		lines = g_strsplit(text, "\n", -1);
		for (i = 0; lines[i] != NULL &&
		     names->len < BAR_MEETING_MAX_SOURCES; i++) {
			gchar *line = g_strstrip(lines[i]);
			guint j;
			gboolean seen = FALSE;

			if (line[0] == '\0' || line[0] == '#')
				continue;
			if (!bar_meeting_name_is_valid(line))
				continue;
			for (j = 0; j < names->len; j++)
				if (strcmp(g_ptr_array_index(names, j),
				           line) == 0)
					seen = TRUE;
			if (!seen)
				g_ptr_array_add(names, g_strdup(line));
		}
	}
	g_ptr_array_add(names, NULL);
	return (gchar **)g_ptr_array_free(names, FALSE);
}

gchar *
bar_meeting_format_selection(const gchar * const *names)
{
	GString *out;
	guint i;

	out = g_string_new("# meeting-notes: sources to record, one per "
	                   "line\n");
	for (i = 0; names != NULL && names[i] != NULL; i++)
		if (bar_meeting_name_is_valid(names[i]))
			g_string_append_printf(out, "%s\n", names[i]);
	return g_string_free(out, FALSE);
}

/*
 * The sound card a source belongs to, from its name: the part after
 * `alsa_input.'/`alsa_output.' up to the first `.' that starts a
 * profile name rather than a PCI function number --- so
 * `alsa_input.pci-0000_c1_00.6.HiFi__Mic1__source' and
 * `alsa_output.pci-0000_c1_00.6.HiFi__Speaker__sink.monitor' are both
 * `pci-0000_c1_00.6'.  NULL for anything not shaped like ALSA's names.
 */
static gchar *
device_key(const gchar *name)
{
	const gchar *p;

	if (g_str_has_prefix(name, "alsa_input."))
		p = name + strlen("alsa_input.");
	else if (g_str_has_prefix(name, "alsa_output."))
		p = name + strlen("alsa_output.");
	else
		return NULL;

	{
		const gchar *q;

		for (q = p; *q != '\0'; q++)
			if (*q == '.' && !g_ascii_isdigit(q[1]))
				break;
		if (q == p)
			return NULL;
		return g_strndup(p, (gsize)(q - p));
	}
}

gboolean
bar_meeting_echo_risk(GPtrArray *selected)
{
	guint i, j;

	if (selected == NULL)
		return FALSE;

	for (i = 0; i < selected->len; i++) {
		BarMeetingSource *mon = g_ptr_array_index(selected, i);
		g_autofree gchar *mkey = NULL;
		g_autofree gchar *lname = NULL;
		g_autofree gchar *ldesc = NULL;
		gboolean speaker;

		if (!mon->monitor)
			continue;
		mkey  = device_key(mon->name);
		lname = g_ascii_strdown(mon->name, -1);
		ldesc = g_ascii_strdown(mon->description != NULL
		                        ? mon->description : "", -1);
		speaker = (strstr(lname, "speaker") != NULL ||
		           strstr(ldesc, "speaker") != NULL);

		for (j = 0; j < selected->len; j++) {
			BarMeetingSource *mic = g_ptr_array_index(selected, j);
			g_autofree gchar *ikey = NULL;

			if (mic->monitor)
				continue;
			if (speaker)
				return TRUE;
			ikey = device_key(mic->name);
			if (mkey != NULL && ikey != NULL &&
			    strcmp(mkey, ikey) == 0)
				return TRUE;
		}
	}
	return FALSE;
}

/* ----------------------------------------------------------------
 * Command templates
 * ---------------------------------------------------------------- */

static const gchar *
lookup_pair(const gchar * const *pairs, const gchar *key, gsize key_len)
{
	guint i;

	for (i = 0; pairs != NULL && pairs[i] != NULL &&
	     pairs[i + 1] != NULL; i += 2)
		if (strlen(pairs[i]) == key_len &&
		    strncmp(pairs[i], key, key_len) == 0)
			return pairs[i + 1];
	return NULL;
}

/* Substitute every known `{name}' in one word, in a single pass. */
static gchar *
substitute_word(const gchar *word, const gchar * const *pairs)
{
	GString *out;
	const gchar *p;

	out = g_string_new(NULL);
	p = word;
	while (*p != '\0') {
		const gchar *close;
		const gchar *value;

		if (*p != '{') {
			g_string_append_c(out, *p++);
			continue;
		}
		close = strchr(p + 1, '}');
		if (close == NULL || close - p > 32) {
			g_string_append_c(out, *p++);
			continue;
		}
		value = lookup_pair(pairs, p + 1, (gsize)(close - p - 1));
		if (value == NULL) {
			/* Unknown: leave it, braces and all.  An ffmpeg
			   filter or a JSON argument may legitimately hold
			   braces. */
			g_string_append_len(out, p, (gssize)(close - p + 1));
		} else {
			g_string_append(out, value);
		}
		p = close + 1;
	}
	return g_string_free(out, FALSE);
}

gchar **
bar_meeting_expand_template(const gchar *tmpl, const gchar * const *pairs,
                            GError **error)
{
	g_auto(GStrv) words = NULL;
	gchar **out;
	gint argc = 0;
	gint i;

	if (tmpl == NULL || tmpl[0] == '\0') {
		g_set_error_literal(error, G_SHELL_ERROR,
		                    G_SHELL_ERROR_EMPTY_STRING,
		                    "the command template is empty");
		return NULL;
	}
	/* Quoting rules only.  The template comes from the user's config,
	   the VALUES come from pactl and the clock --- splitting first and
	   substituting after is what keeps a value from ever becoming two
	   words or an operator. */
	if (!g_shell_parse_argv(tmpl, &argc, &words, error))
		return NULL;

	out = g_new0(gchar *, (gsize)argc + 1);
	for (i = 0; i < argc; i++)
		out[i] = substitute_word(words[i], pairs);
	return out;
}

gchar **
bar_meeting_mix_argv(const gchar *ffmpeg, const gchar * const *inputs,
                     const gchar *output)
{
	GPtrArray *argv;
	guint n, i;

	n = (inputs != NULL) ? g_strv_length((gchar **)inputs) : 0;
	if (n < 1 || output == NULL)
		return NULL;

	argv = g_ptr_array_new();
	g_ptr_array_add(argv, g_strdup((ffmpeg != NULL && ffmpeg[0] != '\0')
	                               ? ffmpeg : "ffmpeg"));
	g_ptr_array_add(argv, g_strdup("-hide_banner"));
	g_ptr_array_add(argv, g_strdup("-loglevel"));
	g_ptr_array_add(argv, g_strdup("error"));
	g_ptr_array_add(argv, g_strdup("-y"));
	for (i = 0; i < n; i++) {
		g_ptr_array_add(argv, g_strdup("-i"));
		g_ptr_array_add(argv, g_strdup(inputs[i]));
	}
	/* One input is only decoded and resampled: an Ogg recording still
	   has to become the WAV whisper reads. */
	if (n > 1) {
		g_ptr_array_add(argv, g_strdup("-filter_complex"));
		g_ptr_array_add(argv, g_strdup_printf(
			"amix=inputs=%u:duration=longest:normalize=0", n));
	}
	g_ptr_array_add(argv, g_strdup("-ar"));
	g_ptr_array_add(argv, g_strdup("16000"));
	g_ptr_array_add(argv, g_strdup("-ac"));
	g_ptr_array_add(argv, g_strdup("1"));
	g_ptr_array_add(argv, g_strdup(output));
	g_ptr_array_add(argv, NULL);
	return (gchar **)g_ptr_array_free(argv, FALSE);
}

/* ----------------------------------------------------------------
 * Names
 * ---------------------------------------------------------------- */

gchar *
bar_meeting_format_clock(gint64 seconds)
{
	if (seconds < 0)
		seconds = 0;
	if (seconds >= 3600)
		return g_strdup_printf("%d:%02d:%02d",
		                       (gint)(seconds / 3600),
		                       (gint)((seconds / 60) % 60),
		                       (gint)(seconds % 60));
	return g_strdup_printf("%02d:%02d", (gint)(seconds / 60),
	                       (gint)(seconds % 60));
}

gchar *
bar_meeting_slug(const gchar *title)
{
	GString *out;
	const gchar *p;
	gboolean gap = FALSE;

	out = g_string_new(NULL);
	if (title == NULL)
		return g_string_free(out, FALSE);

	for (p = title; *p != '\0' && out->len < 40; p++) {
		if (g_ascii_isalnum(*p)) {
			if (gap && out->len > 0)
				g_string_append_c(out, '_');
			gap = FALSE;
			g_string_append_c(out, g_ascii_tolower(*p));
		} else {
			/* Anything else --- spaces, punctuation, every byte
			   of a non-ASCII letter --- is one separator. */
			gap = TRUE;
		}
	}
	if (out->len > 40)
		g_string_truncate(out, 40);
	while (out->len > 0 && out->str[out->len - 1] == '_')
		g_string_truncate(out, out->len - 1);
	return g_string_free(out, FALSE);
}

gchar *
bar_meeting_session_name(GDateTime *when)
{
	return g_date_time_format(when, "%Y-%m-%d_%H-%M-%S");
}

gchar *
bar_meeting_recording_filename(guint index, const gchar *source,
                               const gchar *ext)
{
	GString *safe;
	const gchar *p;

	safe = g_string_new(NULL);
	for (p = (source != NULL) ? source : ""; *p != '\0' &&
	     safe->len < 60; p++) {
		if (g_ascii_isalnum(*p) || *p == '-' || *p == '_')
			g_string_append_c(safe, *p);
		else
			g_string_append_c(safe, '_');
	}
	if (safe->len == 0)
		g_string_append(safe, "source");
	{
		gchar *name = g_strdup_printf("%02u-%s.%s", index + 1,
		                              safe->str,
		                              (ext != NULL && ext[0] != '\0')
		                              ? ext : "ogg");

		g_string_free(safe, TRUE);
		return name;
	}
}

gchar *
bar_meeting_notes_filename(GDateTime *when, const gchar *title,
                           const gchar *format)
{
	g_autofree gchar *stamp = NULL;
	g_autofree gchar *slug = NULL;
	const gchar *ext;

	stamp = g_date_time_format(when, "%Y-%m-%d_%H-%M");
	slug  = bar_meeting_slug(title);
	ext   = (g_strcmp0(format, "md") == 0) ? "md" : "org";
	if (slug[0] == '\0')
		return g_strdup_printf("%s_meeting.%s", stamp, ext);
	return g_strdup_printf("%s_meeting_%s.%s", stamp, slug, ext);
}

gchar *
bar_meeting_default_title(GDateTime *when)
{
	return g_date_time_format(when, "Meeting %Y-%m-%d %H:%M");
}

/* ----------------------------------------------------------------
 * The summary
 * ---------------------------------------------------------------- */

/*
 * If @line is a heading in either syntax, its level and text.  Org
 * `** X' and Markdown `## X' both count; `**bold**' does not, which is
 * why a space after the markers is required.
 */
static gboolean
parse_heading(const gchar *line, gint *level, const gchar **text)
{
	gint n = 0;
	gchar mark;

	if (line[0] != '*' && line[0] != '#')
		return FALSE;
	mark = line[0];
	while (line[n] == mark)
		n++;
	if (line[n] != ' ' || n > 6)
		return FALSE;
	if (level != NULL)
		*level = n;
	if (text != NULL) {
		const gchar *t = line + n;

		while (*t == ' ')
			t++;
		*text = t;
	}
	return TRUE;
}

/* A heading's text, lowered, without trailing colons, bold or space. */
static gchar *
heading_key(const gchar *text)
{
	gchar *key;
	gsize len;

	key = g_ascii_strdown(text, -1);
	g_strstrip(key);
	len = strlen(key);
	while (len > 0 && (key[len - 1] == ':' || key[len - 1] == '*' ||
	                   key[len - 1] == ' '))
		key[--len] = '\0';
	return key;
}

/* Org list or Markdown list line naming an action-items heading in
   prose, e.g. `Action items:' on its own line. */
static gboolean
is_plain_label(const gchar *line, const gchar *label)
{
	g_autofree gchar *key = heading_key(line);

	return g_strcmp0(key, label) == 0;
}

gchar *
bar_meeting_split_summary(const gchar *summary, gchar **actions)
{
	g_auto(GStrv) lines = NULL;
	GString *rest;
	GString *acts;
	gboolean in_actions = FALSE;
	gboolean found = FALSE;
	guint i;

	if (actions != NULL)
		*actions = NULL;
	if (summary == NULL)
		return g_strdup("");

	rest  = g_string_new(NULL);
	acts  = g_string_new(NULL);
	lines = g_strsplit(summary, "\n", -1);

	for (i = 0; lines[i] != NULL; i++) {
		const gchar *line = lines[i];
		const gchar *htext = NULL;
		gboolean heading;
		g_autofree gchar *key = NULL;

		heading = parse_heading(line, NULL, &htext);
		if (!heading && (is_plain_label(line, "action items") ||
		                 is_plain_label(line, "action item"))) {
			heading = TRUE;
			htext = line;
		}

		if (heading) {
			key = heading_key(htext);
			if (g_str_has_prefix(key, "action item")) {
				in_actions = TRUE;
				found = TRUE;
				continue;
			}
			in_actions = FALSE;
			/* The notes already have a Summary section; a
			   heading that only repeats it is noise. */
			if (strcmp(key, "summary") == 0 ||
			    strcmp(key, "tl;dr") == 0 ||
			    strcmp(key, "meeting summary") == 0)
				continue;
		}
		if (in_actions)
			g_string_append_printf(acts, "%s\n", line);
		else
			g_string_append_printf(rest, "%s\n", line);
	}

	if (actions != NULL && found) {
		gchar *a = g_strstrip(g_string_free(acts, FALSE));

		if (a[0] == '\0')
			g_free(a);
		else
			*actions = a;
	} else {
		g_string_free(acts, TRUE);
	}
	return g_strstrip(g_string_free(rest, FALSE));
}

/* ----------------------------------------------------------------
 * The document
 * ---------------------------------------------------------------- */

/*
 * Append @text with any heading re-levelled to @sub_level in the
 * output syntax (`**' for Org, `###' for Markdown), so a model that
 * wrote `* Decisions' does not end the Summary section early.
 */
static void
append_relevelled(GString *out, const gchar *text, gboolean org,
                  gint sub_level)
{
	g_auto(GStrv) lines = NULL;
	guint i;

	lines = g_strsplit(text, "\n", -1);
	for (i = 0; lines[i] != NULL; i++) {
		const gchar *htext;
		gint k;

		if (parse_heading(lines[i], NULL, &htext)) {
			for (k = 0; k < sub_level; k++)
				g_string_append_c(out, org ? '*' : '#');
			g_string_append_printf(out, " %s\n", htext);
		} else {
			g_string_append_printf(out, "%s\n", lines[i]);
		}
	}
}

/*
 * Append a transcript.  Speech is not markup: a line that happens to
 * start with `*' or `#+' would turn into a heading or a keyword in Org,
 * and `#' into a heading in Markdown.  Org gets a leading space (which
 * no heading or keyword may have), Markdown a backslash.
 */
static void
append_transcript(GString *out, const gchar *text, gboolean org)
{
	g_auto(GStrv) lines = NULL;
	guint i;

	lines = g_strsplit(text, "\n", -1);
	for (i = 0; lines[i] != NULL; i++) {
		const gchar *line = lines[i];

		while (*line == ' ' || *line == '\t')
			line++;
		if (line[0] == '\0')
			continue;
		if (org && (line[0] == '*' || line[0] == '#'))
			g_string_append_c(out, ' ');
		else if (!org && line[0] == '#')
			g_string_append_c(out, '\\');
		g_string_append_printf(out, "%s\n", line);
	}
}

/* A YAML double-quoted scalar. */
static gchar *
yaml_quote(const gchar *text)
{
	GString *out;
	const gchar *p;

	out = g_string_new("\"");
	for (p = text; *p != '\0'; p++) {
		if (*p == '"' || *p == '\\')
			g_string_append_c(out, '\\');
		if (*p == '\n')
			g_string_append(out, "\\n");
		else
			g_string_append_c(out, *p);
	}
	g_string_append_c(out, '"');
	return g_string_free(out, FALSE);
}

gchar *
bar_meeting_document(const BarMeetingDoc *doc)
{
	GString *out;
	gboolean org;
	g_autofree gchar *created = NULL;
	g_autofree gchar *clock = NULL;
	g_autofree gchar *description = NULL;
	g_autofree gchar *rest = NULL;
	g_autofree gchar *actions = NULL;
	const gchar *title;
	const gchar *author;

	org    = (g_strcmp0(doc->format, "md") != 0);
	title  = (doc->title != NULL && doc->title[0] != '\0')
	         ? doc->title : "Meeting";
	author = (doc->author != NULL && doc->author[0] != '\0')
	         ? doc->author : "unknown";
	/* %:z is the offset with its colon, which is what ISO 8601 and the
	   notes repository's headers use. */
	created = g_date_time_format(doc->created, "%Y-%m-%dT%H:%M:%S%:z");
	clock   = bar_meeting_format_clock(doc->duration_s);
	description = g_strdup_printf(
		"Meeting notes: %s recorded from %u source%s", clock,
		doc->n_sources, (doc->n_sources == 1) ? "" : "s");

	if (doc->summary != NULL && doc->summary[0] != '\0')
		rest = bar_meeting_split_summary(doc->summary, &actions);

	out = g_string_new(NULL);
	if (org) {
		g_string_append_printf(out,
			"#+title: %s\n"
			"#+description: %s\n"
			"#+authors: %s\n"
			"#+categories: meeting\n"
			"#+created: %s\n"
			"#+updated: %s\n"
			"#+version: 1.0\n\n",
			title, description, author, created, created);
	} else {
		g_autofree gchar *qt = yaml_quote(title);
		g_autofree gchar *qd = yaml_quote(description);
		g_autofree gchar *qa = yaml_quote(author);

		g_string_append_printf(out,
			"---\n"
			"title: %s\n"
			"description: %s\n"
			"authors: %s\n"
			"categories: [meeting]\n"
			"created: %s\n"
			"updated: %s\n"
			"version: 1.0\n"
			"---\n\n"
			"# %s\n\n",
			qt, qd, qa, created, created, title);
	}

	g_string_append(out, org ? "* Summary\n" : "## Summary\n\n");
	if (rest != NULL && rest[0] != '\0')
		append_relevelled(out, rest, org, org ? 2 : 3);
	else
		g_string_append_printf(out, "%s\n",
			(doc->summary_note != NULL)
				? doc->summary_note
				: "No summary was generated.");

	g_string_append(out, org ? "\n* Action items\n"
	                         : "\n## Action items\n\n");
	if (actions != NULL)
		append_relevelled(out, actions, org, org ? 2 : 3);
	else
		g_string_append(out, (rest != NULL)
			? "None captured.\n"
			: "None captured --- there is no summary to take "
			  "them from.\n");

	g_string_append(out, org ? "\n* Transcript\n"
	                         : "\n## Transcript\n\n");
	if (doc->transcript != NULL && doc->transcript[0] != '\0')
		append_transcript(out, doc->transcript, org);
	else
		g_string_append(out, "(empty)\n");
	if (doc->recordings != NULL)
		g_string_append_printf(out, org
			? "\nRecordings: [[file:%s]]\n"
			: "\nRecordings: <file://%s>\n", doc->recordings);

	g_string_append(out, org ? "\n* My notes\n\n" : "\n## My notes\n\n");
	return g_string_free(out, FALSE);
}

/* ----------------------------------------------------------------
 * The cmacs form
 * ---------------------------------------------------------------- */

/* Text that can travel as one Elisp string literal through D-Bus. */
static gboolean
form_text_ok(const gchar *text)
{
	const gchar *p;

	if (text == NULL)
		return TRUE;
	if (!g_utf8_validate(text, -1, NULL))
		return FALSE;
	for (p = text; *p != '\0'; p++)
		if (((guchar)*p < 0x20 && *p != '\t') || (guchar)*p == 0x7f)
			return FALSE;
	return TRUE;
}

static void
append_lisp_string(GString *out, const gchar *text)
{
	g_autofree gchar *quoted = NULL;

	if (text == NULL || text[0] == '\0') {
		g_string_append(out, "nil");
		return;
	}
	quoted = bar_elisp_quote(text);
	g_string_append_printf(out, "\"%s\"", quoted);
}

gchar *
bar_meeting_cmacs_form(const gchar *widget, const gchar *job_id,
                       const gchar *session_dir, const gchar * const *files,
                       const gchar *title, const gchar *destination)
{
	GString *out;
	guint i;

	if (widget == NULL || job_id == NULL || session_dir == NULL ||
	    files == NULL || files[0] == NULL)
		return NULL;
	if (!form_text_ok(widget) || !form_text_ok(job_id) ||
	    !form_text_ok(session_dir) || !form_text_ok(title) ||
	    !form_text_ok(destination))
		return NULL;
	for (i = 0; files[i] != NULL; i++)
		if (!form_text_ok(files[i]) || files[i][0] == '\0')
			return NULL;

	out = g_string_new("(cmacs-gowl-bar-meeting-finish ");
	append_lisp_string(out, widget);
	g_string_append_c(out, ' ');
	append_lisp_string(out, job_id);
	g_string_append_c(out, ' ');
	append_lisp_string(out, session_dir);
	g_string_append(out, " (list");
	for (i = 0; files[i] != NULL; i++) {
		g_string_append_c(out, ' ');
		append_lisp_string(out, files[i]);
	}
	g_string_append(out, ") ");
	append_lisp_string(out, title);
	g_string_append_c(out, ' ');
	append_lisp_string(out, destination);
	g_string_append_c(out, ')');
	return g_string_free(out, FALSE);
}

/* ----------------------------------------------------------------
 * Untrusted text
 * ---------------------------------------------------------------- */

gchar *
bar_meeting_clean_text(const gchar *text, gsize max_bytes)
{
	g_autofree gchar *valid = NULL;
	GString *out;
	const gchar *p;
	const gchar *end;

	if (text == NULL)
		return g_strdup("");

	valid = g_utf8_make_valid(text, -1);
	out = g_string_new(NULL);
	end = valid + strlen(valid);
	p = valid;
	while (p < end) {
		const gchar *next = g_utf8_next_char(p);

		if ((gsize)(out->len + (gsize)(next - p)) > max_bytes)
			break;
		g_string_append_len(out, p, (gssize)(next - p));
		p = next;
	}
	return g_string_free(out, FALSE);
}
