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
 * The cpu widget's activity mode (modules/bar/bar-activity-model.c and
 * the cpu section of modules/bar/bar-plugins-system.c).
 *
 * Asserted:
 *
 *   NOTHING CHANGES BY DEFAULT.  With no settings the widget still
 *   reads `CPU N%', carries no tooltip and adds nothing to its
 *   signature; every activity feature is behind a setting.
 *
 *   A SIGNAL NEVER LANDS ON THE WRONG PROCESS.  Before Terminate or
 *   Force kill, the process is re-read: a pid whose start time moved
 *   (reused), another user's process, init and the compositor itself
 *   are all refused with a reason.  Force kill takes two clicks.
 *
 *   /proc IS READ AS THE KERNEL WRITES IT.  A command name holding
 *   spaces and parentheses, bytes that are not UTF-8, negative
 *   priorities, offline cores and a process that vanishes mid-read.
 *
 *   THE JOURNAL STAYS BOUNDED.  Seven days and 20 MiB, enforced by
 *   pruning whole lines, and exported as CSV with a header.
 *
 * Fixtures are fake /proc and /sys trees under
 * tests/fixtures/bar/cpu-activity/; the end-to-end signal tests use a
 * real `sleep' child of this test, never anything else.
 */

#include <glib.h>
#include <glib/gstdio.h>
#include <signal.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define BAR_HARNESS_MODULE GOWL_TEST_BAR_MODULE
#include "bar-widget-harness.h"

#ifndef GOWL_TEST_BAR_MODULE
#define GOWL_TEST_BAR_MODULE "build/release/modules/bar.so"
#endif
#ifndef GOWL_TEST_FIXTURES
#define GOWL_TEST_FIXTURES "tests/fixtures/bar"
#endif

/* The model's own types, so the helpers are called exactly as bar.so
   exports them. */
#include "../modules/bar/bar-activity-model.h"

#define GROUP "bar_register_system_plugins"

/* A mirror of BarEnv (modules/bar/bar-internal.h).  The cpu widget
   reads the shared sysinfo reader through it; without one a poll does
   nothing, as it does before the module has started. */
typedef struct {
	gpointer sysinfo;
	gpointer compositor;
	gpointer registry;
} EnvMirror;

static EnvMirror env_mirror;
static gchar *test_home;

/* ── Looking up the model ─────────────────────────────────────────── */

typedef struct {
	gint      (*sampling)      (const gchar *);
	gboolean  (*fahrenheit)    (const gchar *);
	gdouble   (*c_to_f)        (gdouble);
	void      (*format_temp)   (gint, gboolean, gchar *, gsize);
	gboolean  (*creature)      (const gchar *);
	gint      (*clamp)         (const gchar *, gint, gint, gint);
	void      (*compute)       (gint, gint, gint, gint, gint,
	                            BarActivityCreature *);
	gint      (*side)          (gint);
	void      (*layout)        (gint, const BarActivityCreature *,
	                            BarActivityCreatureGeom *);
	gint      (*wiggle)        (gint64, gint, gboolean, gboolean);
	gboolean  (*resolve)       (const GowlBarTheme *, const gchar *,
	                            GowlBarColor, gdouble *);
	gboolean  (*parse_stat)    (const gchar *, BarActivityCpuTimes *,
	                            GArray **);
	gint      (*cpu_percent)   (const BarActivityCpuTimes *,
	                            const BarActivityCpuTimes *);
	GArray   *(*freqs)         (const gchar *);
	guint     (*band)          (gint);
	void      (*grid)          (guint, guint *, guint *);
	gboolean  (*pid_stat)      (const gchar *, BarActivityProc *);
	gboolean  (*pid_status)    (const gchar *, guint *, guint64 *);
	GPtrArray*(*read_procs)    (const gchar *, gint);
	void      (*compute_cpu)   (GPtrArray *, GHashTable *, gdouble,
	                            glong);
	void      (*sort)          (GPtrArray *);
	gboolean  (*matches)       (const BarActivityProc *, const gchar *);
	BarActivityKillCheck (*kill_check) (const gchar *, gint, guint64,
	                                    guint, guint, gint);
	gboolean  (*proc_id)       (const gchar *, const gchar *, gint *,
	                            guint64 *, guint *);
	gchar   **(*nvidia_argv)   (const gchar *, gint);
	GPtrArray*(*nvidia_csv)    (const gchar *);
	GPtrArray*(*mounts)        (const gchar *);
	gchar    *(*journal_line)  (gint64, gint, gint, gint);
	gboolean  (*journal_append)(const gchar *, const gchar *, GError **);
	gboolean  (*journal_prune) (const gchar *, gint64, gint64, gsize,
	                            GError **);
	gboolean  (*journal_export)(const gchar *, const gchar *, GError **);
	gchar    *(*export_path)   (const gchar *, GDateTime *);
	void      (*proc_free)     (BarActivityProc *);
} Model;

#define SYM(field, name) \
	do { \
		m->field = bar_harness_sym(h, name); \
		g_assert_nonnull(m->field); \
	} while (0)

static gboolean
model_up(BarHarness *h, Model *m)
{
	if (!bar_harness_up(h, GROUP, NULL))
		return FALSE;
	SYM(sampling,       "bar_activity_sampling_interval");
	SYM(fahrenheit,     "bar_activity_parse_fahrenheit");
	SYM(c_to_f,         "bar_activity_c_to_f");
	SYM(format_temp,    "bar_activity_format_temp");
	SYM(creature,       "bar_activity_parse_style_creature");
	SYM(clamp,          "bar_activity_clamp_setting");
	SYM(compute,        "bar_activity_creature_compute");
	SYM(side,           "bar_activity_creature_side");
	SYM(layout,         "bar_activity_creature_layout");
	SYM(wiggle,         "bar_activity_wiggle_offset");
	SYM(resolve,        "bar_activity_resolve_color");
	SYM(parse_stat,     "bar_activity_parse_proc_stat");
	SYM(cpu_percent,    "bar_activity_cpu_percent");
	SYM(freqs,          "bar_activity_read_frequencies");
	SYM(band,           "bar_activity_core_band");
	SYM(grid,           "bar_activity_die_grid");
	SYM(pid_stat,       "bar_activity_parse_pid_stat");
	SYM(pid_status,     "bar_activity_parse_pid_status");
	SYM(read_procs,     "bar_activity_read_processes");
	SYM(compute_cpu,    "bar_activity_compute_cpu");
	SYM(sort,           "bar_activity_sort_processes");
	SYM(matches,        "bar_activity_proc_matches");
	SYM(kill_check,     "bar_activity_kill_check");
	SYM(proc_id,        "bar_activity_parse_proc_id");
	SYM(nvidia_argv,    "bar_activity_nvidia_argv");
	SYM(nvidia_csv,     "bar_activity_parse_nvidia_csv");
	SYM(mounts,         "bar_activity_parse_mounts");
	SYM(journal_line,   "bar_activity_journal_line");
	SYM(journal_append, "bar_activity_journal_append");
	SYM(journal_prune,  "bar_activity_journal_prune");
	SYM(journal_export, "bar_activity_journal_export");
	SYM(export_path,    "bar_activity_export_path");
	SYM(proc_free,      "bar_activity_proc_free");
	return TRUE;
}

static gchar *
fixture(const gchar *name)
{
	return g_build_filename(GOWL_TEST_FIXTURES, "cpu-activity", name,
	                        NULL);
}

static gchar *
fixture_text(const gchar *name)
{
	g_autofree gchar *path = fixture(name);
	gchar *text = NULL;

	g_assert_true(g_file_get_contents(path, &text, NULL, NULL));
	return text;
}

/* ── The widget, with a real sysinfo reader behind it ─────────────── */

static gboolean
widget_up(BarHarness *h)
{
	gpointer (*sysinfo_new)(void);
	void (*env_set)(gconstpointer);

	if (!bar_harness_up(h, GROUP, "cpu"))
		return FALSE;
	sysinfo_new = bar_harness_sym(h, "bar_sysinfo_new");
	env_set     = bar_harness_sym(h, "bar_env_set");
	g_assert_nonnull(sysinfo_new);
	g_assert_nonnull(env_set);
	memset(&env_mirror, 0, sizeof(env_mirror));
	env_mirror.sysinfo = sysinfo_new();
	env_set(&env_mirror);
	bar_harness_activate(h);
	return TRUE;
}

static void
widget_down(BarHarness *h)
{
	void (*env_set)(gconstpointer);
	void (*sysinfo_free)(gpointer);

	env_set      = bar_harness_sym(h, "bar_env_set");
	sysinfo_free = bar_harness_sym(h, "bar_sysinfo_free");
	bar_harness_down(h);
	if (env_set != NULL)
		env_set(NULL);
	if (sysinfo_free != NULL)
		sysinfo_free(env_mirror.sysinfo);
	env_mirror.sysinfo = NULL;
}

/* The first panel item whose id starts with @prefix, or NULL. */
static GowlBarPanelItem *
find_item(GowlBarPanel *panel, const gchar *prefix)
{
	guint i;

	for (i = 0; i < gowl_bar_panel_n_items(panel); i++) {
		GowlBarPanelItem *it = gowl_bar_panel_get_item(panel, i);
		const gchar *id = gowl_bar_panel_item_get_id(it);

		if (id != NULL && g_str_has_prefix(id, prefix))
			return it;
	}
	return NULL;
}

static GowlBarPanelItem *
find_kind(GowlBarPanel *panel, GowlBarItemKind kind)
{
	guint i;

	for (i = 0; i < gowl_bar_panel_n_items(panel); i++) {
		GowlBarPanelItem *it = gowl_bar_panel_get_item(panel, i);

		if (gowl_bar_panel_item_get_kind(it) == kind)
			return it;
	}
	return NULL;
}

/* A button row's child labels, joined, for asserting what it offers. */
static gchar *
buttons_text(GowlBarPanelItem *row)
{
	GString *s = g_string_new(NULL);
	guint i;

	for (i = 0; i < gowl_bar_panel_item_n_children(row); i++) {
		g_string_append(s, gowl_bar_panel_item_get_title(
			gowl_bar_panel_item_get_child(row, i)));
		g_string_append_c(s, '|');
	}
	return g_string_free(s, FALSE);
}

/* ── Settings ─────────────────────────────────────────────────────── */

/* Catches: a sampling name mapped to the wrong period, or an unknown
   one (a typo in a config) producing 0 --- which the bar reads as
   "poll every tick". */
static void
test_sampling_speeds(void)
{
	BarHarness h;
	Model m;

	if (!model_up(&h, &m))
		return;
	g_assert_cmpint(m.sampling("efficient"), ==, 4);
	g_assert_cmpint(m.sampling("balanced"), ==, 2);
	g_assert_cmpint(m.sampling("fast"), ==, 1);
	g_assert_cmpint(m.sampling(NULL), ==, 2);
	g_assert_cmpint(m.sampling("ludicrous"), ==, 2);
	bar_harness_down(&h);

	/* And through the widget, which is what the bar asks. */
	if (!widget_up(&h))
		return;
	g_assert_cmpint(gowl_bar_plugin_get_interval(h.plugin), ==, 2);
	bar_harness_set(&h, "sampling", "fast");
	g_assert_cmpint(gowl_bar_plugin_get_interval(h.plugin), ==, 1);
	bar_harness_set(&h, "sampling", "efficient");
	g_assert_cmpint(gowl_bar_plugin_get_interval(h.plugin), ==, 4);
	widget_down(&h);
}

/* Catches: a wrong conversion constant, a rounding that truncates
   125.6 to 125, or the unit setting accepting nothing but one exact
   spelling. */
static void
test_temperature_units(void)
{
	BarHarness h;
	Model m;
	gchar buf[32];

	if (!model_up(&h, &m))
		return;
	g_assert_cmpfloat_with_epsilon(m.c_to_f(100.0), 212.0, 1e-9);
	g_assert_cmpfloat_with_epsilon(m.c_to_f(0.0), 32.0, 1e-9);
	g_assert_cmpfloat_with_epsilon(m.c_to_f(-40.0), -40.0, 1e-9);

	m.format_temp(52000, FALSE, buf, sizeof(buf));
	g_assert_cmpstr(buf, ==, "52\xc2\xb0""C");
	m.format_temp(52000, TRUE, buf, sizeof(buf));
	g_assert_cmpstr(buf, ==, "126\xc2\xb0""F");

	g_assert_true(m.fahrenheit("fahrenheit"));
	g_assert_true(m.fahrenheit("Fahrenheit"));
	g_assert_true(m.fahrenheit("f"));
	g_assert_false(m.fahrenheit("celsius"));
	g_assert_false(m.fahrenheit(NULL));
	bar_harness_down(&h);
}

/* Catches: `style' parsed so loosely that a typo turns the creature
   on, or numeric settings that accept garbage and out-of-range values
   instead of falling back and clamping. */
static void
test_style_and_numeric_settings(void)
{
	BarHarness h;
	Model m;

	if (!model_up(&h, &m))
		return;
	g_assert_true(m.creature("creature"));
	g_assert_true(m.creature("Creature"));
	g_assert_false(m.creature("label"));
	g_assert_false(m.creature(NULL));
	g_assert_false(m.creature("creatures"));

	g_assert_cmpint(m.clamp(NULL, 50, 0, 99), ==, 50);
	g_assert_cmpint(m.clamp("", 50, 0, 99), ==, 50);
	g_assert_cmpint(m.clamp("70", 50, 0, 99), ==, 70);
	g_assert_cmpint(m.clamp("150", 50, 0, 99), ==, 99);
	g_assert_cmpint(m.clamp("-5", 50, 0, 99), ==, 0);
	g_assert_cmpint(m.clamp("7O", 50, 0, 99), ==, 50);
	g_assert_cmpint(m.clamp("99999999999999999999", 60, 10, 600), ==,
	                600);
	bar_harness_down(&h);
}

/* ── The creature ─────────────────────────────────────────────────── */

/* Catches: rows lit below the threshold, a jump from nothing to three
   rows when it is crossed, more than six rows at saturation, and eyes
   that do not track CPU (left) and memory (right). */
static void
test_creature_fill_maths(void)
{
	BarHarness h;
	Model m;
	BarActivityCreature c;

	if (!model_up(&h, &m))
		return;

	m.compute(0, 0, 0, 50, 80, &c);
	g_assert_cmpfloat(c.left_eye, ==, 0.0);
	g_assert_cmpfloat(c.right_eye, ==, 0.0);
	g_assert_cmpuint(c.rows, ==, 0);
	g_assert_true(c.calm);
	g_assert_false(c.hot);

	m.compute(50, 50, 50, 50, 80, &c);
	g_assert_cmpfloat_with_epsilon(c.left_eye, 0.5, 1e-9);
	g_assert_cmpfloat_with_epsilon(c.right_eye, 0.5, 1e-9);
	g_assert_cmpuint(c.rows, ==, 0);    /* AT the threshold: calm */
	g_assert_true(c.calm);

	m.compute(50, 20, 51, 50, 80, &c);
	g_assert_cmpuint(c.rows, ==, 1);    /* just past it: one row */
	g_assert_false(c.calm);

	m.compute(50, 20, 75, 50, 80, &c);
	g_assert_cmpuint(c.rows, ==, 3);

	m.compute(100, 100, 100, 50, 80, &c);
	g_assert_cmpfloat(c.left_eye, ==, 1.0);
	g_assert_cmpfloat(c.right_eye, ==, 1.0);
	g_assert_cmpuint(c.rows, ==, BAR_ACTIVITY_CREATURE_ROWS);
	g_assert_true(c.hot);

	/* Load over 100% (more runnable tasks than cores) saturates. */
	m.compute(10, 10, 400, 50, 80, &c);
	g_assert_cmpuint(c.rows, ==, BAR_ACTIVITY_CREATURE_ROWS);

	/* Heat: from the threshold, not past it. */
	m.compute(79, 0, 0, 50, 80, &c);
	g_assert_false(c.hot);
	m.compute(80, 0, 0, 50, 80, &c);
	g_assert_true(c.hot);

	/* A threshold of 0 lights a row for any load at all. */
	m.compute(0, 0, 1, 0, 80, &c);
	g_assert_cmpuint(c.rows, ==, 1);
	m.compute(0, 0, 0, 0, 80, &c);
	g_assert_cmpuint(c.rows, ==, 0);

	bar_harness_down(&h);
}

static gboolean
rect_inside(const BarActivityRect *r, gdouble side)
{
	return r->x >= 0.0 && r->y >= 0.0 && r->w >= 0.0 && r->h >= 0.0 &&
	       r->x + r->w <= side + 1e-9 && r->y + r->h <= side + 1e-9;
}

/* Catches: the creature growing past the bar, row 0 not being the
   bottom one, eyes filling from the top, and flames drawn on the
   body instead of above it. */
static void
test_creature_geometry(void)
{
	BarHarness h;
	Model m;
	BarActivityCreature c;
	BarActivityCreatureGeom g;
	gint heights[] = { 4, 20, 28, 48 };
	guint i, k;

	if (!model_up(&h, &m))
		return;

	g_assert_cmpint(m.side(28), ==, 22);
	g_assert_cmpint(m.side(4), ==, 10);   /* never vanishes */

	for (k = 0; k < G_N_ELEMENTS(heights); k++) {
		m.compute(50, 100, 100, 50, 80, &c);
		m.layout(heights[k], &c, &g);
		g_assert_cmpint(g.side, ==, m.side(heights[k]));

		g_assert_true(rect_inside(&g.body, g.side));
		for (i = 0; i < 2; i++) {
			g_assert_true(rect_inside(&g.eye[i], g.side));
			/* fills share the socket's bottom edge */
			g_assert_cmpfloat_with_epsilon(
				g.eye_fill[i].y + g.eye_fill[i].h,
				g.eye[i].y + g.eye[i].h, 1e-9);
		}
		g_assert_cmpfloat_with_epsilon(g.eye_fill[0].h,
		                               g.eye[0].h * 0.5, 1e-9);
		g_assert_cmpfloat_with_epsilon(g.eye_fill[1].h,
		                               g.eye[1].h, 1e-9);
		g_assert_true(g.eye[0].x + g.eye[0].w <= g.eye[1].x);

		for (i = 0; i < BAR_ACTIVITY_CREATURE_ROWS; i++) {
			g_assert_true(rect_inside(&g.row[i], g.side));
			g_assert_true(g.row[i].y >= g.body.y);
			g_assert_true(g.row[i].y > g.eye[0].y + g.eye[0].h);
			if (i > 0)  /* each row sits above the last */
				g_assert_cmpfloat(g.row[i].y, <,
				                  g.row[i - 1].y);
		}
		for (i = 0; i < BAR_ACTIVITY_CREATURE_FLAMES; i++) {
			g_assert_true(rect_inside(&g.flame[i], g.side));
			g_assert_cmpfloat(g.flame[i].y + g.flame[i].h, <=,
			                  g.body.y);
		}
	}

	/* Empty eyes are empty. */
	m.compute(0, 0, 0, 50, 80, &c);
	m.layout(28, &c, &g);
	g_assert_cmpfloat(g.eye_fill[0].h, ==, 0.0);
	g_assert_cmpfloat(g.eye_fill[1].h, ==, 0.0);
	bar_harness_down(&h);
}

/* Catches: a creature that shakes while calm, one that shakes with
   motion off, and a shake that does not alternate tick to tick ---
   which, with the offset in the signature, is the only thing that
   moves it without a timer. */
static void
test_creature_wiggle(void)
{
	BarHarness h;
	Model m;
	gint64 s = G_USEC_PER_SEC;

	if (!model_up(&h, &m))
		return;
	g_assert_cmpint(m.wiggle(0, 2, FALSE, TRUE), ==, 0);
	g_assert_cmpint(m.wiggle(0, 2, TRUE, FALSE), ==, 0);
	g_assert_cmpint(m.wiggle(0, 2, TRUE, TRUE), ==, -1);
	g_assert_cmpint(m.wiggle(1 * s, 2, TRUE, TRUE), ==, -1);
	g_assert_cmpint(m.wiggle(2 * s, 2, TRUE, TRUE), ==, 1);
	g_assert_cmpint(m.wiggle(4 * s, 2, TRUE, TRUE), ==, -1);
	g_assert_cmpint(m.wiggle(1 * s, 1, TRUE, TRUE), ==, 1);
	g_assert_cmpint(m.wiggle(5 * s, 0, TRUE, TRUE), ==, 1);  /* 0 -> 1 */
	bar_harness_down(&h);
}

/* Catches: a `color-*' setting that takes a role but drops a hex
   literal (or the reverse), and garbage that paints black instead of
   falling back to the role. */
static void
test_creature_colour_settings(void)
{
	BarHarness h;
	Model m;
	g_autoptr(GowlBarTheme) theme = gowl_bar_theme_new();
	gdouble rgba[4];
	const gdouble *red, *subtext;

	if (!model_up(&h, &m))
		return;
	red     = gowl_bar_theme_color(theme, GOWL_BAR_COLOR_RED);
	subtext = gowl_bar_theme_color(theme, GOWL_BAR_COLOR_SUBTEXT);

	g_assert_true(m.resolve(theme, "red", GOWL_BAR_COLOR_SUBTEXT, rgba));
	g_assert_cmpmem(rgba, sizeof(rgba), red, 4 * sizeof(gdouble));

	g_assert_true(m.resolve(theme, "#00ff00", GOWL_BAR_COLOR_SUBTEXT,
	                        rgba));
	g_assert_cmpfloat_with_epsilon(rgba[0], 0.0, 1e-9);
	g_assert_cmpfloat_with_epsilon(rgba[1], 1.0, 1e-9);
	g_assert_cmpfloat_with_epsilon(rgba[2], 0.0, 1e-9);

	g_assert_false(m.resolve(theme, "not-a-colour",
	                         GOWL_BAR_COLOR_SUBTEXT, rgba));
	g_assert_cmpmem(rgba, sizeof(rgba), subtext, 4 * sizeof(gdouble));
	g_assert_false(m.resolve(theme, NULL, GOWL_BAR_COLOR_SUBTEXT, rgba));
	g_assert_cmpmem(rgba, sizeof(rgba), subtext, 4 * sizeof(gdouble));
	bar_harness_down(&h);
}

/* ── /proc/stat, cpufreq and the die ──────────────────────────────── */

/* Catches: per-core lines indexed by position rather than core number
   (an offline cpu2 shifting cpu3 into its slot), iowait counted as
   busy, and a hostile core number sizing a huge array. */
static void
test_per_core_parse_and_delta(void)
{
	BarHarness h;
	Model m;
	g_autofree gchar *a = NULL;
	g_autofree gchar *b = NULL;
	g_autofree gchar *bad = NULL;
	g_autoptr(GArray) ca = NULL;
	g_autoptr(GArray) cb = NULL;
	g_autoptr(GArray) cbad = NULL;
	BarActivityCpuTimes ta, tb;

	if (!model_up(&h, &m))
		return;
	a   = fixture_text("proc_stat_a");
	b   = fixture_text("proc_stat_b");
	bad = fixture_text("proc_stat_hostile");

	g_assert_true(m.parse_stat(a, &ta, &ca));
	g_assert_true(m.parse_stat(b, &tb, &cb));
	g_assert_cmpuint(ta.total, ==, 94850);
	g_assert_cmpuint(ta.idle, ==, 81000);
	g_assert_cmpint(m.cpu_percent(&ta, &tb), ==, 57);

	g_assert_cmpuint(ca->len, ==, 4);   /* cpu0..cpu3, cpu2 offline */
	g_assert_cmpint(m.cpu_percent(
		&g_array_index(ca, BarActivityCpuTimes, 0),
		&g_array_index(cb, BarActivityCpuTimes, 0)), ==, 80);
	g_assert_cmpint(m.cpu_percent(
		&g_array_index(ca, BarActivityCpuTimes, 1),
		&g_array_index(cb, BarActivityCpuTimes, 1)), ==, 0);
	g_assert_cmpuint(g_array_index(ca, BarActivityCpuTimes, 2).total,
	                 ==, 0);
	g_assert_cmpint(m.cpu_percent(
		&g_array_index(ca, BarActivityCpuTimes, 3),
		&g_array_index(cb, BarActivityCpuTimes, 3)), ==, 50);

	/* No time passed, or counters went backwards: no figure. */
	g_assert_cmpint(m.cpu_percent(&ta, &ta), ==, -1);
	g_assert_cmpint(m.cpu_percent(&tb, &ta), ==, -1);

	g_assert_false(m.parse_stat(bad, &ta, &cbad));
	g_assert_cmpuint(cbad->len, ==, 0);
	g_assert_false(m.parse_stat(NULL, NULL, NULL));
	bar_harness_down(&h);
}

/* Catches: kHz shown as MHz, `cpufreq' and `cpuidle' read as cores,
   and a machine without cpufreq reported as clocks of -1. */
static void
test_frequency_parse(void)
{
	BarHarness h;
	Model m;
	g_autofree gchar *sys = NULL;
	g_autofree gchar *none = NULL;
	g_autoptr(GArray) f = NULL;
	g_autoptr(GArray) empty = NULL;
	g_autoptr(GArray) missing = NULL;

	if (!model_up(&h, &m))
		return;
	sys  = fixture("sys");
	none = fixture("sys-nofreq");

	f = m.freqs(sys);
	g_assert_cmpuint(f->len, ==, 4);
	g_assert_cmpint(g_array_index(f, gint, 0), ==, 2800);
	g_assert_cmpint(g_array_index(f, gint, 1), ==, 3400);
	g_assert_cmpint(g_array_index(f, gint, 2), ==, -1);
	g_assert_cmpint(g_array_index(f, gint, 3), ==, 800);

	empty = m.freqs(none);
	g_assert_cmpuint(empty->len, ==, 0);
	missing = m.freqs("/nonexistent-sysfs-root");
	g_assert_cmpuint(missing->len, ==, 0);
	bar_harness_down(&h);
}

/* Catches: a die colour key that disagrees with the legend's
   thresholds, and grids that leave cores out or go 128 wide. */
static void
test_die_bands_and_grid(void)
{
	BarHarness h;
	Model m;
	guint c, r;

	if (!model_up(&h, &m))
		return;
	g_assert_cmpuint(m.band(0), ==, 0);
	g_assert_cmpuint(m.band(24), ==, 0);
	g_assert_cmpuint(m.band(25), ==, 1);
	g_assert_cmpuint(m.band(50), ==, 2);
	g_assert_cmpuint(m.band(75), ==, 3);
	g_assert_cmpuint(m.band(89), ==, 3);
	g_assert_cmpuint(m.band(90), ==, 4);
	g_assert_cmpuint(m.band(100), ==, 4);

	m.grid(0, &c, &r);
	g_assert_cmpuint(c, ==, 0);
	g_assert_cmpuint(r, ==, 0);
	m.grid(4, &c, &r);
	g_assert_cmpuint(c, ==, 4);
	g_assert_cmpuint(r, ==, 1);
	m.grid(12, &c, &r);
	g_assert_cmpuint(c, ==, 8);
	g_assert_cmpuint(r, ==, 2);
	m.grid(128, &c, &r);
	g_assert_cmpuint(c, ==, 16);
	g_assert_cmpuint(r, ==, 8);
	m.grid(33, &c, &r);
	g_assert_cmpuint(c * r, >=, 33);
	bar_harness_down(&h);
}

/* ── Processes ────────────────────────────────────────────────────── */

/* Catches: splitting /proc/PID/stat on spaces or on the first `)',
   which reads a process called `(foo) bar)' from inside its own name
   --- wrong state, wrong times, wrong start time, and so a wrong
   pid-reuse verdict. */
static void
test_pid_stat_parse(void)
{
	BarHarness h;
	Model m;
	g_autofree gchar *weird = NULL;
	g_autofree gchar *rt = NULL;
	g_autofree gchar *hostile = NULL;
	BarActivityProc p;

	if (!model_up(&h, &m))
		return;
	weird   = fixture_text("proc/4242/stat");
	rt      = fixture_text("proc/4244/stat");
	hostile = fixture_text("proc/4245/stat");

	memset(&p, 0, sizeof(p));
	g_assert_true(m.pid_stat(weird, &p));
	g_assert_cmpint(p.pid, ==, 4242);
	g_assert_cmpstr(p.name, ==, "(foo) bar)");
	g_assert_cmpint(p.state, ==, 'R');
	g_assert_cmpuint(p.ticks, ==, 1500);
	g_assert_cmpuint(p.starttime, ==, 555);
	g_free(p.name);

	/* A negative priority (realtime) does not stop the walk. */
	memset(&p, 0, sizeof(p));
	g_assert_true(m.pid_stat(rt, &p));
	g_assert_cmpuint(p.starttime, ==, 1200);
	g_assert_cmpuint(p.ticks, ==, 15);
	g_free(p.name);

	/* Not UTF-8, a control byte and a newline: valid UTF-8 with no
	   control characters, and the fields after it still right. */
	memset(&p, 0, sizeof(p));
	g_assert_true(m.pid_stat(hostile, &p));
	g_assert_true(g_utf8_validate(p.name, -1, NULL));
	g_assert_null(strchr(p.name, '\n'));
	g_assert_null(strchr(p.name, '\x01'));
	g_assert_cmpint(p.state, ==, 'Z');
	g_assert_cmpuint(p.starttime, ==, 7777);
	g_free(p.name);

	memset(&p, 0, sizeof(p));
	g_assert_false(m.pid_stat("", &p));
	g_assert_false(m.pid_stat("12 (x", &p));
	g_assert_false(m.pid_stat("12 (x) S 1 2", &p));     /* truncated */
	g_assert_false(m.pid_stat("-3 (x) S 1 1 1 1 1 1 1 1 1 1 1 1 1 1 "
	                          "1 1 1 1 1", &p));
	g_assert_false(m.pid_stat(NULL, &p));
	bar_harness_down(&h);
}

/* Catches: the effective uid read instead of the real one, VmRSS
   missing (a kernel thread or a zombie) treated as a parse failure. */
static void
test_pid_status_parse(void)
{
	BarHarness h;
	Model m;
	g_autofree gchar *bash = NULL;
	g_autofree gchar *zombie = NULL;
	guint uid = 0;
	guint64 rss = 99;

	if (!model_up(&h, &m))
		return;
	bash   = fixture_text("proc/4243/status");
	zombie = fixture_text("proc/4245/status");

	g_assert_true(m.pid_status(bash, &uid, &rss));
	g_assert_cmpuint(uid, ==, 1000);
	g_assert_cmpuint(rss, ==, 5120);

	g_assert_true(m.pid_status(zombie, &uid, &rss));
	g_assert_cmpuint(rss, ==, 0);

	g_assert_true(m.pid_status("Uid:\t7\t0\t0\t0\n", &uid, NULL));
	g_assert_cmpuint(uid, ==, 7);
	g_assert_false(m.pid_status("Name:\tx\n", &uid, &rss));
	g_assert_false(m.pid_status(NULL, &uid, &rss));
	bar_harness_down(&h);
}

static gboolean
has_pid(GPtrArray *procs, gint pid)
{
	guint i;

	for (i = 0; i < procs->len; i++)
		if (((BarActivityProc *)g_ptr_array_index(procs, i))->pid == pid)
			return TRUE;
	return FALSE;
}

/* Catches: other users' processes in the list (and so in reach of
   Terminate), non-numeric /proc entries read as processes, and a
   process that vanished mid-read failing the whole sample. */
static void
test_read_processes_uid_filter(void)
{
	BarHarness h;
	Model m;
	g_autofree gchar *root = NULL;
	g_autoptr(GPtrArray) all = NULL;
	g_autoptr(GPtrArray) mine = NULL;
	g_autoptr(GPtrArray) nobody = NULL;
	guint i;

	if (!model_up(&h, &m))
		return;
	root = fixture("proc");

	all = m.read_procs(root, -1);
	g_assert_cmpuint(all->len, ==, 5);   /* 1 4242 4243 4244 4245 */
	g_assert_false(has_pid(all, 4246));  /* stat without status */

	mine = m.read_procs(root, 1000);
	g_assert_cmpuint(mine->len, ==, 3);
	g_assert_true(has_pid(mine, 4242));
	g_assert_true(has_pid(mine, 4243));
	g_assert_true(has_pid(mine, 4245));
	g_assert_false(has_pid(mine, 1));
	g_assert_false(has_pid(mine, 4244));
	for (i = 0; i < mine->len; i++) {
		BarActivityProc *p = g_ptr_array_index(mine, i);

		g_assert_cmpuint(p->uid, ==, 1000);
		g_assert_cmpfloat(p->cpu, ==, -1.0);
	}

	nobody = m.read_procs("/nonexistent-procfs", -1);
	g_assert_cmpuint(nobody->len, ==, 0);
	bar_harness_down(&h);
}

static BarActivityProc *
mkproc(gint pid, guint64 ticks, guint64 start, guint64 rss)
{
	BarActivityProc *p = g_new0(BarActivityProc, 1);

	p->pid       = pid;
	p->name      = g_strdup_printf("p%d", pid);
	p->ticks     = ticks;
	p->starttime = start;
	p->rss_kb    = rss;
	p->cpu       = -1.0;
	return p;
}

/* Catches: CPU computed from absolute ticks rather than the delta, a
   reused pid inheriting the previous process's baseline (a new
   process reading as 4000%), and a baseline table that never forgets
   exited processes. */
static void
test_process_cpu_delta(void)
{
	BarHarness h;
	Model m;
	g_autoptr(GHashTable) prev = NULL;
	g_autoptr(GPtrArray) s1 = NULL;
	g_autoptr(GPtrArray) s2 = NULL;
	BarActivityProc *busy, *idle, *reused, *fresh;

	if (!model_up(&h, &m))
		return;
	prev = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);

	s1 = g_ptr_array_new_with_free_func((GDestroyNotify)m.proc_free);
	g_ptr_array_add(s1, mkproc(10, 1000, 5, 100));
	g_ptr_array_add(s1, mkproc(11, 500, 6, 100));
	g_ptr_array_add(s1, mkproc(12, 9000, 7, 100));
	g_ptr_array_add(s1, mkproc(99, 1, 8, 1));      /* exits */
	m.compute_cpu(s1, prev, 0.0, 100);
	g_assert_cmpfloat(((BarActivityProc *)s1->pdata[0])->cpu, ==, -1.0);
	g_assert_cmpuint(g_hash_table_size(prev), ==, 4);

	/* Two seconds later at 100 ticks a second. */
	s2 = g_ptr_array_new_with_free_func((GDestroyNotify)m.proc_free);
	busy   = mkproc(10, 1300, 5, 100);    /* +300 -> 150% */
	idle   = mkproc(11, 500, 6, 300);     /* +0   ->   0% */
	reused = mkproc(12, 40, 70, 100);     /* same pid, new start */
	fresh  = mkproc(13, 50, 71, 100);
	g_ptr_array_add(s2, idle);
	g_ptr_array_add(s2, reused);
	g_ptr_array_add(s2, busy);
	g_ptr_array_add(s2, fresh);
	m.compute_cpu(s2, prev, 2.0, 100);
	g_assert_cmpfloat_with_epsilon(busy->cpu, 150.0, 1e-9);
	g_assert_cmpfloat_with_epsilon(idle->cpu, 0.0, 1e-9);
	g_assert_cmpfloat(reused->cpu, ==, -1.0);
	g_assert_cmpfloat(fresh->cpu, ==, -1.0);
	g_assert_cmpuint(g_hash_table_size(prev), ==, 4);
	g_assert_false(g_hash_table_contains(prev, "99:8"));

	/* Busiest first; ties broken by memory, then pid. */
	m.sort(s2);
	g_assert_true(s2->pdata[0] == busy);
	g_assert_true(s2->pdata[1] == idle);
	g_assert_true(s2->pdata[2] == reused);  /* -1, pid 12 < 13 */
	g_assert_true(s2->pdata[3] == fresh);

	/* No time passed: no rate. */
	m.compute_cpu(s2, prev, 0.0, 100);
	g_assert_cmpfloat(busy->cpu, ==, -1.0);
	bar_harness_down(&h);
}

/* Catches: a filter that is case-sensitive or cannot find a pid. */
static void
test_process_filter(void)
{
	BarHarness h;
	Model m;
	BarActivityProc *p;

	if (!model_up(&h, &m))
		return;
	p = mkproc(4243, 0, 0, 0);
	g_free(p->name);
	p->name = g_strdup("Firefox");
	g_assert_true(m.matches(p, NULL));
	g_assert_true(m.matches(p, ""));
	g_assert_true(m.matches(p, "fire"));
	g_assert_true(m.matches(p, "FOX"));
	g_assert_true(m.matches(p, "424"));
	g_assert_false(m.matches(p, "chrome"));
	m.proc_free(p);
	bar_harness_down(&h);
}

/* Catches: a signal sent to a pid that now names another program,
   another user's process, init, or the compositor itself. */
static void
test_kill_check(void)
{
	BarHarness h;
	Model m;
	g_autofree gchar *root = NULL;

	if (!model_up(&h, &m))
		return;
	root = fixture("proc");

	g_assert_cmpint(m.kill_check(root, 4243, 987654, 1000, 1000, 77), ==,
	                BAR_ACTIVITY_KILL_OK);
	/* The start time moved: the pid was reused. */
	g_assert_cmpint(m.kill_check(root, 4243, 987653, 1000, 1000, 77), ==,
	                BAR_ACTIVITY_KILL_REUSED);
	/* Gone. */
	g_assert_cmpint(m.kill_check(root, 9999, 1, 1000, 1000, 77), ==,
	                BAR_ACTIVITY_KILL_GONE);
	/* Root's sshd, even if a stale sample claimed it was ours. */
	g_assert_cmpint(m.kill_check(root, 4244, 1200, 1000, 1000, 77), ==,
	                BAR_ACTIVITY_KILL_NOT_OWNER);
	/* A sample for another user is refused before /proc is read. */
	g_assert_cmpint(m.kill_check(root, 4243, 987654, 0, 1000, 77), ==,
	                BAR_ACTIVITY_KILL_NOT_OWNER);
	/* Init, nothing at all, and ourselves. */
	g_assert_cmpint(m.kill_check(root, 1, 2, 0, 0, 77), ==,
	                BAR_ACTIVITY_KILL_PROTECTED);
	g_assert_cmpint(m.kill_check(root, 0, 0, 1000, 1000, 77), ==,
	                BAR_ACTIVITY_KILL_PROTECTED);
	g_assert_cmpint(m.kill_check(root, -1, 0, 1000, 1000, 77), ==,
	                BAR_ACTIVITY_KILL_PROTECTED);
	g_assert_cmpint(m.kill_check(root, 4243, 987654, 1000, 1000, 4243),
	                ==, BAR_ACTIVITY_KILL_PROTECTED);
	bar_harness_down(&h);
}

/* Catches: an item id that smuggles extra fields, a negative pid or a
   prefix confusion (a `proc:' row id accepted as a `sig:' action). */
static void
test_proc_id_parse(void)
{
	BarHarness h;
	Model m;
	gint pid = 0;
	guint64 start = 0;
	guint uid = 0;

	if (!model_up(&h, &m))
		return;
	g_assert_true(m.proc_id("sig:42:900:1000", "sig", &pid, &start,
	                        &uid));
	g_assert_cmpint(pid, ==, 42);
	g_assert_cmpuint(start, ==, 900);
	g_assert_cmpuint(uid, ==, 1000);

	g_assert_false(m.proc_id("proc:42:900:1000", "sig", &pid, &start,
	                         &uid));
	g_assert_false(m.proc_id("sig:42:900", "sig", &pid, &start, &uid));
	g_assert_false(m.proc_id("sig:42:900:1000:5", "sig", &pid, &start,
	                         &uid));
	g_assert_false(m.proc_id("sig:-1:900:1000", "sig", &pid, &start,
	                         &uid));
	g_assert_false(m.proc_id("sig:0:900:1000", "sig", &pid, &start,
	                         &uid));
	g_assert_false(m.proc_id("sig:4x:900:1000", "sig", &pid, &start,
	                         &uid));
	g_assert_false(m.proc_id(NULL, "sig", &pid, &start, &uid));
	bar_harness_down(&h);
}

/* ── GPU and filesystems ──────────────────────────────────────────── */

/* Catches: a command setting spliced into a shell line, `[N/A]'
   failing a whole board, a comma in a board name shifting every field,
   and stray lines read as boards. */
static void
test_nvidia_argv_and_csv(void)
{
	BarHarness h;
	Model m;
	g_auto(GStrv) argv = NULL;
	g_auto(GStrv) bare = NULL;
	g_autofree gchar *csv = NULL;
	g_autoptr(GPtrArray) gpus = NULL;
	g_autoptr(GPtrArray) none = NULL;
	BarActivityGpu *g0, *g1;

	if (!model_up(&h, &m))
		return;

	argv = m.nvidia_argv("nvidia-smi; rm -rf ~", 5);
	g_assert_cmpstr(argv[0], ==, "timeout");
	g_assert_cmpstr(argv[1], ==, "5");
	g_assert_cmpstr(argv[2], ==, "nvidia-smi; rm -rf ~");  /* one arg */
	g_assert_true(g_str_has_prefix(argv[3], "--query-gpu=name,"));
	g_assert_cmpstr(argv[4], ==, "--format=csv,noheader,nounits");
	g_assert_null(argv[5]);

	bare = m.nvidia_argv(NULL, 0);
	g_assert_cmpstr(bare[0], ==, "nvidia-smi");

	csv  = fixture_text("nvidia.csv");
	gpus = m.nvidia_csv(csv);
	g_assert_cmpuint(gpus->len, ==, 2);
	g0 = g_ptr_array_index(gpus, 0);
	g1 = g_ptr_array_index(gpus, 1);
	g_assert_cmpstr(g0->name, ==, "NVIDIA GeForce RTX 4070");
	g_assert_cmpint(g0->util, ==, 37);
	g_assert_cmpint(g0->mem_used_mb, ==, 2048);
	g_assert_cmpint(g0->mem_total_mb, ==, 12282);
	g_assert_cmpint(g0->temp_c, ==, 61);
	g_assert_cmpstr(g1->name, ==, "Acme, Inc. Board");
	g_assert_cmpint(g1->util, ==, -1);
	g_assert_cmpint(g1->mem_used_mb, ==, 100);
	g_assert_cmpint(g1->temp_c, ==, -1);

	none = m.nvidia_csv(NULL);
	g_assert_cmpuint(none->len, ==, 0);
	bar_harness_down(&h);
}

/* Catches: network filesystems in the list (a statvfs on a hung NFS
   server blocks the worker for minutes), one disk listed once per
   btrfs subvolume, an ostree system disk named `/sysroot' or `/etc'
   (whichever /proc/mounts listed first) instead of `/var', and escaped
   spaces left as `\040'. */
static void
test_mounts_parse(void)
{
	BarHarness h;
	Model m;
	g_autofree gchar *text = NULL;
	g_autoptr(GPtrArray) mounts = NULL;
	GString *seen;
	guint i;

	if (!model_up(&h, &m))
		return;
	text   = fixture_text("mounts");
	mounts = m.mounts(text);
	seen   = g_string_new(NULL);
	for (i = 0; i < mounts->len; i++) {
		BarActivityMount *mt = g_ptr_array_index(mounts, i);

		g_string_append_printf(seen, "%s=%s;", mt->mount, mt->fstype);
	}
	g_assert_cmpstr(seen->str, ==,
	                "/=btrfs;/boot=ext4;/boot/efi=vfat;"
	                "/run/media/zach/My Disk=exfat;/var=btrfs;");
	g_string_free(seen, TRUE);
	bar_harness_down(&h);
}

/* ── The journal ──────────────────────────────────────────────────── */

static gchar *
scratch_file(BarHarness *h, const gchar *name, const gchar *content)
{
	gchar *path = bar_harness_path(h, name);

	if (content != NULL)
		g_assert_true(g_file_set_contents(path, content, -1, NULL));
	return path;
}

/* Catches: a journal that keeps last week forever, one that keeps a
   torn half-line from a crash, and one rewritten when nothing changed
   (a write every poll for nothing). */
static void
test_journal_prune_by_age(void)
{
	BarHarness h;
	Model m;
	g_autofree gchar *path = NULL;
	g_autofree gchar *text = NULL;
	gint64 now = 1760000000;
	GStatBuf before, after;

	if (!model_up(&h, &m))
		return;
	path = scratch_file(&h, "age.journal",
		"1759000000,10,20,40\n"     /* 11.5 days old */
		"1759395201,11,21,41\n"     /* inside seven days */
		"garbage,1,2,3\n"
		"1759999999,12,22,\n");
	g_assert_true(m.journal_prune(path, now,
	                              BAR_ACTIVITY_JOURNAL_MAX_AGE,
	                              BAR_ACTIVITY_JOURNAL_MAX_BYTES, NULL));
	g_assert_true(g_file_get_contents(path, &text, NULL, NULL));
	g_assert_cmpstr(text, ==, "1759395201,11,21,41\n1759999999,12,22,\n");

	/* Nothing to drop: the file is not rewritten. */
	g_assert_cmpint(g_stat(path, &before), ==, 0);
	g_usleep(10000);
	g_assert_true(m.journal_prune(path, now,
	                              BAR_ACTIVITY_JOURNAL_MAX_AGE,
	                              BAR_ACTIVITY_JOURNAL_MAX_BYTES, NULL));
	g_assert_cmpint(g_stat(path, &after), ==, 0);
	g_assert_cmpint(before.st_ino, ==, after.st_ino);

	/* No journal at all is fine. */
	g_assert_true(m.journal_prune("/nonexistent/dir/x.journal", now,
	                              10, 10, NULL));
	bar_harness_down(&h);
}

/* Catches: a size cap enforced by cutting mid-line (the file then
   starts with half a record), or by dropping the NEWEST lines. */
static void
test_journal_prune_by_size(void)
{
	BarHarness h;
	Model m;
	g_autofree gchar *path = NULL;
	g_autofree gchar *text = NULL;
	GString *big;
	gint64 now = 1760000000;
	gint i;

	if (!model_up(&h, &m))
		return;
	big = g_string_new(NULL);
	for (i = 0; i < 100; i++)   /* 100 lines of 20 bytes each */
		g_string_append_printf(big, "%" G_GINT64_FORMAT ",%02d,50,40\n",
		                       now - 100 + i, i);
	g_assert_cmpuint(big->len, ==, 2000);
	path = scratch_file(&h, "size.journal", big->str);
	g_string_free(big, TRUE);

	g_assert_true(m.journal_prune(path, now, 3600, 210, NULL));
	g_assert_true(g_file_get_contents(path, &text, NULL, NULL));
	g_assert_cmpuint(strlen(text), ==, 200);   /* 10 whole lines */
	g_assert_true(g_str_has_prefix(text, "1759999990,90,"));
	g_assert_true(g_str_has_suffix(text, "1759999999,99,50,40\n"));
	bar_harness_down(&h);
}

/* Catches: a journal created world-readable, an append that
   truncates, and an export without its header or with raw epochs. */
static void
test_journal_append_and_export(void)
{
	BarHarness h;
	Model m;
	g_autofree gchar *path = NULL;
	g_autofree gchar *dest = NULL;
	g_autofree gchar *out = NULL;
	g_autofree gchar *l1 = NULL;
	g_autofree gchar *l2 = NULL;
	g_autoptr(GDateTime) day = NULL;
	g_autofree gchar *named = NULL;
	g_auto(GStrv) rows = NULL;
	GStatBuf st;

	if (!model_up(&h, &m))
		return;
	path = bar_harness_path(&h, "nested/dir/activity.journal");
	dest = bar_harness_path(&h, "export.csv");

	l1 = m.journal_line(1760000000, 23, 41, 52500);
	l2 = m.journal_line(1760000002, 140, -3, 0);
	g_assert_cmpstr(l1, ==, "1760000000,23,41,52\n");
	g_assert_cmpstr(l2, ==, "1760000002,100,0,\n");   /* clamped */

	g_assert_true(m.journal_append(path, l1, NULL));
	g_assert_true(m.journal_append(path, l2, NULL));
	g_assert_cmpint(g_stat(path, &st), ==, 0);
	g_assert_cmpint(st.st_mode & 0777, ==, 0600);

	g_assert_true(m.journal_export(path, dest, NULL));
	g_assert_true(g_file_get_contents(dest, &out, NULL, NULL));
	rows = g_strsplit(out, "\n", -1);
	g_assert_cmpstr(rows[0], ==,
	                "timestamp,cpu_percent,memory_percent,temperature_c");
	g_assert_true(g_str_has_suffix(rows[1], ",23,41,52"));
	g_assert_true(strchr(rows[1], 'T') != NULL);   /* ISO 8601 */
	g_assert_true(g_str_has_suffix(rows[2], ",100,0,"));
	g_assert_cmpstr(rows[3], ==, "");

	g_assert_false(m.journal_export("/nonexistent/j", dest, NULL));

	day   = g_date_time_new_local(2026, 10, 16, 9, 0, 0);
	named = m.export_path("/home/someone", day);
	g_assert_cmpstr(named, ==,
	                "/home/someone/activity-journal-2026-10-16.csv");
	bar_harness_down(&h);
}

/* ── The widget ───────────────────────────────────────────────────── */

/* Catches: the activity mode leaking into the default widget --- a
   label that is no longer `CPU N%', a tooltip, or creature state in
   the signature. */
static void
test_label_style_unchanged_by_default(void)
{
	BarHarness h;
	g_autofree gchar *label = NULL;
	g_autofree gchar *tip = NULL;
	g_autoptr(GString) sig = g_string_new(NULL);
	g_autoptr(GRegex) re = NULL;

	if (!widget_up(&h))
		return;
	bar_harness_poll(&h);
	label = gowl_bar_plugin_dup_label(h.plugin);
	re = g_regex_new("^CPU [0-9]{1,3}%$", 0, 0, NULL);
	g_assert_nonnull(label);
	g_assert_true(g_regex_match(re, label, 0, NULL));
	tip = gowl_bar_plugin_dup_tooltip(h.plugin);
	g_assert_null(tip);

	gowl_bar_plugin_signature(h.plugin, sig);
	g_assert_null(strstr(sig->str, "creature"));

	/* `style: label' spelt out is the same widget. */
	bar_harness_set(&h, "style", "label");
	bar_harness_poll(&h);
	g_clear_pointer(&label, g_free);
	label = gowl_bar_plugin_dup_label(h.plugin);
	g_assert_true(g_regex_match(re, label, 0, NULL));
	widget_down(&h);
}

/* Catches: a creature the bar never repaints (nothing in the
   signature), one that measures as a text label, and a draw that
   crashes on a hot creature with colour overrides. */
static void
test_creature_style_draws(void)
{
	BarHarness h;
	g_autoptr(GString) sig = g_string_new(NULL);
	g_autofree gchar *tip = NULL;
	g_autoptr(GowlBarTheme) theme = gowl_bar_theme_new();
	cairo_surface_t *surface;
	cairo_t *cr;
	PangoLayout *layout;
	gint width, pad;

	if (!widget_up(&h))
		return;
	bar_harness_set(&h, "style", "creature");
	/* Thresholds at their extremes, so it is hot and full. */
	bar_harness_set(&h, "heat-threshold", "1");
	bar_harness_set(&h, "load-threshold", "0");
	bar_harness_set(&h, "color-body", "#ff8800");
	bar_harness_set(&h, "color-flame", "mauve");
	bar_harness_set(&h, "color-eye", "nonsense");
	bar_harness_poll(&h);

	gowl_bar_plugin_signature(h.plugin, sig);
	g_assert_nonnull(strstr(sig->str, "creature:"));
	tip = gowl_bar_plugin_dup_tooltip(h.plugin);
	g_assert_nonnull(tip);
	g_assert_true(g_str_has_prefix(tip, "CPU "));
	g_assert_nonnull(strstr(tip, "Memory"));

	surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 200, 30);
	cr = cairo_create(surface);
	layout = pango_cairo_create_layout(cr);
	pad = gowl_bar_theme_metric(theme, GOWL_BAR_METRIC_ITEM_PAD);
	width = gowl_bar_plugin_measure(h.plugin, layout, theme, 30);
	g_assert_cmpint(width, ==, (30 - 6) + 2 * pad);
	gowl_bar_plugin_draw(h.plugin, cr, layout, theme, 0, 0, width, 30,
	                     TRUE, FALSE);
	g_assert_cmpint(cairo_status(cr), ==, CAIRO_STATUS_SUCCESS);

	/* Something was painted inside the slot. */
	{
		guchar *px;
		gint stride, x, y;
		gboolean painted = FALSE;

		cairo_surface_flush(surface);
		px = cairo_image_surface_get_data(surface);
		stride = cairo_image_surface_get_stride(surface);
		for (y = 0; y < 30 && !painted; y++)
			for (x = 0; x < width && !painted; x++)
				if (px[y * stride + x * 4 + 3] != 0)
					painted = TRUE;
		g_assert_true(painted);
	}

	g_object_unref(layout);
	cairo_destroy(cr);
	cairo_surface_destroy(surface);

	/* Back to the label: the tooltip goes with the creature. */
	bar_harness_set(&h, "style", "label");
	bar_harness_poll(&h);
	g_clear_pointer(&tip, g_free);
	tip = gowl_bar_plugin_dup_tooltip(h.plugin);
	g_assert_null(tip);
	widget_down(&h);
}

/* Catches: `history-samples' ignored (the graph stays 48 long) or not
   bounded (a setting of 10^9 allocating gigabytes). */
static void
test_history_samples(void)
{
	BarHarness h;
	GowlBarPanel *panel;
	GowlBarPanelItem *graph;
	guint n = 0, i;

	if (!widget_up(&h))
		return;
	bar_harness_set(&h, "history-samples", "10");
	for (i = 0; i < 15; i++)
		bar_harness_poll(&h);
	panel = bar_harness_panel(&h);
	graph = find_kind(panel, GOWL_BAR_ITEM_GRAPH);
	g_assert_nonnull(graph);
	gowl_bar_panel_item_get_samples(graph, &n);
	g_assert_cmpuint(n, ==, 10);
	g_object_unref(panel);

	bar_harness_set(&h, "history-samples", "1000000000");
	for (i = 0; i < 3; i++)
		bar_harness_poll(&h);
	panel = bar_harness_panel(&h);
	graph = find_kind(panel, GOWL_BAR_ITEM_GRAPH);
	gowl_bar_panel_item_get_samples(graph, &n);
	g_assert_cmpuint(n, ==, 3);   /* resized to 600, refilling */
	g_object_unref(panel);
	widget_down(&h);
}

/* Catches: the compact panel losing what the system panel always had
   (graph, Monitor/Processes/Disks), the die missing, and Expand not
   adding the GPU, filesystems and the filtered process list. */
static void
test_panel_compact_and_expanded(void)
{
	BarHarness h;
	GowlBarPanel *panel;
	GowlBarPanelItem *view, *tools;
	g_autofree gchar *view_text = NULL;
	g_autofree gchar *tool_text = NULL;

	if (!widget_up(&h))
		return;
	bar_harness_poll(&h);
	bar_harness_poll(&h);
	panel = bar_harness_panel(&h);
	g_assert_true(bar_harness_panel_mentions(panel, "Processor"));
	g_assert_true(bar_harness_panel_mentions(panel, "CPU die"));
	g_assert_false(bar_harness_panel_mentions(panel, "Filesystems"));
	g_assert_null(find_item(panel, "filter"));
	view  = find_item(panel, "view");
	tools = find_item(panel, "tool");
	g_assert_nonnull(view);
	g_assert_nonnull(tools);
	view_text = buttons_text(view);
	tool_text = buttons_text(tools);
	g_assert_cmpstr(view_text, ==, "Expand|");
	g_assert_cmpstr(tool_text, ==, "Monitor|Processes|Disks|");
	g_assert_null(find_item(panel, "journal"));
	/* Wide enough for the die and a process table side by side with
	   their numbers; GOWL_TEST_DUMP_PANELS=dir writes it out. */
	g_assert_cmpint(gowl_bar_panel_get_width(panel), >=, 600);
	bar_harness_dump_panel(&h, panel, "cpu-compact");
	g_object_unref(panel);

	gowl_bar_plugin_panel_opened(h.plugin);
	gowl_bar_plugin_panel_action(h.plugin, "view", 0, 0.0, 1);
	panel = bar_harness_panel(&h);
	g_assert_true(bar_harness_panel_mentions(panel, "GPU"));
	g_assert_true(bar_harness_panel_mentions(panel, "Filesystems"));
	g_assert_true(bar_harness_panel_mentions(panel, "Processes"));
	g_assert_nonnull(find_item(panel, "filter"));
	g_assert_nonnull(find_item(panel, "proc:"));   /* we exist */
	g_clear_pointer(&view_text, g_free);
	view_text = buttons_text(find_item(panel, "view"));
	g_assert_cmpstr(view_text, ==, "Compact|");
	bar_harness_poll(&h);
	g_object_unref(panel);
	panel = bar_harness_panel(&h);
	bar_harness_dump_panel(&h, panel, "cpu-expanded");
	g_object_unref(panel);

	/* Compact again, and it stays compact across polls. */
	gowl_bar_plugin_panel_action(h.plugin, "view", 0, 0.0, 1);
	bar_harness_poll(&h);
	panel = bar_harness_panel(&h);
	g_assert_false(bar_harness_panel_mentions(panel, "Filesystems"));
	g_object_unref(panel);
	gowl_bar_plugin_panel_closed(h.plugin);
	widget_down(&h);
}

/* Catches: `open-expanded' not opening expanded, or re-applied every
   poll so the Compact button is undone a second later. */
static void
test_open_expanded_setting(void)
{
	BarHarness h;
	GowlBarPanel *panel;

	if (!widget_up(&h))
		return;
	bar_harness_set(&h, "open-expanded", "true");
	panel = bar_harness_panel(&h);
	g_assert_true(bar_harness_panel_mentions(panel, "Filesystems"));
	g_object_unref(panel);

	gowl_bar_plugin_panel_action(h.plugin, "view", 0, 0.0, 1);
	bar_harness_poll(&h);
	bar_harness_poll(&h);
	panel = bar_harness_panel(&h);
	g_assert_false(bar_harness_panel_mentions(panel, "Filesystems"));
	g_object_unref(panel);
	widget_down(&h);
}

/* Catches: Fahrenheit set but the panel still showing Celsius. */
static void
test_panel_temperature_unit(void)
{
	BarHarness h;
	GowlBarPanel *panel;
	gint (*temp_mc)(gpointer);

	if (!widget_up(&h))
		return;
	temp_mc = bar_harness_sym(&h, "bar_sysinfo_temp_mc");
	if (temp_mc(env_mirror.sysinfo) <= 0) {
		g_test_skip("no CPU temperature sensor on this machine");
		widget_down(&h);
		return;
	}
	bar_harness_poll(&h);
	panel = bar_harness_panel(&h);
	g_assert_true(bar_harness_panel_mentions(panel, "\xc2\xb0""C"));
	g_object_unref(panel);

	bar_harness_set(&h, "temperature-unit", "fahrenheit");
	panel = bar_harness_panel(&h);
	g_assert_true(bar_harness_panel_mentions(panel, "\xc2\xb0""F"));
	g_assert_false(bar_harness_panel_mentions(panel, "\xc2\xb0""C"));
	g_object_unref(panel);
	widget_down(&h);
}

/* Catches: show-frequencies with nothing behind it, and clocks shown
   when it is off. */
static void
test_panel_frequencies(void)
{
	BarHarness h;
	GowlBarPanel *panel;

	if (!widget_up(&h))
		return;
	panel = bar_harness_panel(&h);
	g_assert_false(bar_harness_panel_mentions(panel, "Clock"));
	g_object_unref(panel);

	bar_harness_set(&h, "show-frequencies", "true");
	gowl_bar_plugin_panel_opened(h.plugin);   /* samples inline */
	panel = bar_harness_panel(&h);
	g_assert_true(bar_harness_panel_mentions(panel, "Clock"));
	g_assert_true(bar_harness_panel_mentions(panel, "MHz") ||
	              bar_harness_panel_mentions(panel, "not reported"));
	g_object_unref(panel);
	gowl_bar_plugin_panel_closed(h.plugin);
	widget_down(&h);
}

/* Catches: the nvidia-smi command setting not honoured, its CSV not
   reaching the panel, and the missing-tool explanation lacking the
   package to install. */
static void
test_panel_nvidia(void)
{
	BarHarness h;
	GowlBarPanel *panel;
	g_autofree gchar *fake = NULL;
	g_autofree gchar *csv = NULL;
	g_autofree gchar *body = NULL;
	gint (*gpu_percent)(gpointer);

	if (!widget_up(&h))
		return;
	csv  = fixture("nvidia.csv");
	body = g_strdup_printf("cat '%s'", csv);
	fake = bar_harness_script(&h, "fake-nvidia-smi", body);
	bar_harness_set(&h, "nvidia-smi", fake);
	bar_harness_set(&h, "open-expanded", "true");
	gowl_bar_plugin_panel_opened(h.plugin);
	panel = bar_harness_panel(&h);
	g_assert_true(bar_harness_panel_mentions(panel, "RTX 4070"));
	g_assert_true(bar_harness_panel_mentions(panel, "2048 / 12282 MB"));
	g_object_unref(panel);
	gowl_bar_plugin_panel_closed(h.plugin);
	widget_down(&h);

	/* Not installed.  Only checkable where amdgpu is not reporting,
	   since then the panel has a reading and needs no excuse. */
	if (!widget_up(&h))
		return;
	gpu_percent = bar_harness_sym(&h, "bar_sysinfo_gpu_percent");
	bar_harness_set(&h, "nvidia-smi", "/nonexistent/nvidia-smi");
	bar_harness_set(&h, "open-expanded", "true");
	gowl_bar_plugin_panel_opened(h.plugin);
	panel = bar_harness_panel(&h);
	if (gpu_percent(env_mirror.sysinfo) < 0)
		g_assert_true(bar_harness_panel_mentions(panel,
			"xorg-x11-drv-nvidia-cuda"));
	else
		g_test_message("amdgpu reports here; the missing-tool text "
		               "is not shown and was not checked");
	g_object_unref(panel);
	gowl_bar_plugin_panel_closed(h.plugin);
	widget_down(&h);
}

/* A `sleep' child of this test, the only thing these tests signal. */
static GPid
spawn_sleeper(void)
{
	const gchar *argv[] = { "sleep", "30", NULL };
	GPid pid = 0;

	g_assert_true(g_spawn_async(NULL, (gchar **)argv, NULL,
		G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD,
		NULL, NULL, &pid, NULL));
	/* Let it exec, so its comm reads `sleep'. */
	g_usleep(50000);
	return pid;
}

/* Expand, filter to @pid, and return the row id the panel shows for
   it (owned by the caller). */
static gchar *
row_for(BarHarness *h, GPid pid)
{
	GowlBarPanel *panel;
	GowlBarPanelItem *row;
	g_autofree gchar *filter = NULL;
	g_autofree gchar *prefix = NULL;
	gchar *id;

	filter = g_strdup_printf("%d", (gint)pid);
	prefix = g_strdup_printf("proc:%d:", (gint)pid);
	gowl_bar_plugin_panel_text(h->plugin, "filter", filter, FALSE);
	gowl_bar_plugin_poll_async(h->plugin);   /* a fresh sample */
	panel = bar_harness_panel(h);
	row = find_item(panel, prefix);
	g_assert_nonnull(row);
	id = g_strdup(gowl_bar_panel_item_get_id(row));
	g_object_unref(panel);
	return id;
}

static gboolean
alive(GPid pid)
{
	gint status;

	return waitpid(pid, &status, WNOHANG) == 0;
}

/* Catches: Terminate signalling without re-reading the process (a
   forged start time --- what a reused pid looks like --- must be
   refused and the process left running), and Terminate not sending
   SIGTERM to the right process. */
static void
test_terminate_end_to_end(void)
{
	BarHarness h;
	GowlBarPanel *panel;
	GowlBarPanelItem *sig;
	g_autofree gchar *row = NULL;
	g_autofree gchar *forged = NULL;
	g_autofree gchar *sig_id = NULL;
	g_autofree gchar *buttons = NULL;
	gint pid, status = 0;
	guint64 start;
	guint uid;
	gboolean (*proc_id)(const gchar *, const gchar *, gint *, guint64 *,
	                    guint *);
	GPid child;

	if (!widget_up(&h))
		return;
	proc_id = bar_harness_sym(&h, "bar_activity_parse_proc_id");
	child = spawn_sleeper();
	bar_harness_set(&h, "open-expanded", "true");
	gowl_bar_plugin_panel_opened(h.plugin);

	row = row_for(&h, child);
	g_assert_true(proc_id(row, "proc", &pid, &start, &uid));
	g_assert_cmpint(pid, ==, (gint)child);
	g_assert_cmpuint(uid, ==, getuid());

	/* Selecting the row offers the actions under it. */
	gowl_bar_plugin_panel_action(h.plugin, row, -1, 0.0, 1);
	sig_id = g_strdup_printf("sig:%d:%" G_GUINT64_FORMAT ":%u", pid,
	                         start, uid);
	panel = bar_harness_panel(&h);
	sig = find_item(panel, sig_id);
	g_assert_nonnull(sig);
	buttons = buttons_text(sig);
	g_assert_cmpstr(buttons, ==, "Terminate|Force kill|");
	g_object_unref(panel);

	/* A start time that does not match: refused, still running. */
	forged = g_strdup_printf("sig:%d:%" G_GUINT64_FORMAT ":%u", pid,
	                         start + 1, uid);
	gowl_bar_plugin_panel_action(h.plugin, forged, 0, 0.0, 1);
	g_assert_true(bar_harness_toasted(&h, "different program"));
	g_assert_true(alive(child));

	/* The real thing. */
	gowl_bar_plugin_panel_action(h.plugin, sig_id, 0, 0.0, 1);
	g_assert_true(bar_harness_toasted(&h, "Sent SIGTERM"));
	g_assert_cmpint(waitpid(child, &status, 0), ==, child);
	g_assert_true(WIFSIGNALED(status));
	g_assert_cmpint(WTERMSIG(status), ==, SIGTERM);

	gowl_bar_plugin_panel_closed(h.plugin);
	widget_down(&h);
}

/* Catches: Force kill on one click, an arm that never expires being
   shared with another row, and the second click not sending SIGKILL. */
static void
test_force_kill_two_step(void)
{
	BarHarness h;
	GowlBarPanel *panel;
	g_autofree gchar *row = NULL;
	g_autofree gchar *sig_id = NULL;
	g_autofree gchar *buttons = NULL;
	gint pid, status = 0;
	guint64 start;
	guint uid;
	gboolean (*proc_id)(const gchar *, const gchar *, gint *, guint64 *,
	                    guint *);
	GPid child;

	if (!widget_up(&h))
		return;
	proc_id = bar_harness_sym(&h, "bar_activity_parse_proc_id");
	child = spawn_sleeper();
	bar_harness_set(&h, "open-expanded", "true");
	gowl_bar_plugin_panel_opened(h.plugin);
	row = row_for(&h, child);
	g_assert_true(proc_id(row, "proc", &pid, &start, &uid));
	gowl_bar_plugin_panel_action(h.plugin, row, -1, 0.0, 1);
	sig_id = g_strdup_printf("sig:%d:%" G_GUINT64_FORMAT ":%u", pid,
	                         start, uid);

	/* First click: armed, nothing sent. */
	gowl_bar_plugin_panel_action(h.plugin, sig_id, 1, 0.0, 1);
	g_assert_true(alive(child));
	g_assert_cmpuint(h.host->toasts->len, ==, 0);
	panel = bar_harness_panel(&h);
	buttons = buttons_text(find_item(panel, sig_id));
	g_assert_cmpstr(buttons, ==, "Terminate|Click again to force kill|");
	g_object_unref(panel);

	/* Second click: SIGKILL. */
	gowl_bar_plugin_panel_action(h.plugin, sig_id, 1, 0.0, 1);
	g_assert_true(bar_harness_toasted(&h, "Sent SIGKILL"));
	g_assert_cmpint(waitpid(child, &status, 0), ==, child);
	g_assert_true(WIFSIGNALED(status));
	g_assert_cmpint(WTERMSIG(status), ==, SIGKILL);

	gowl_bar_plugin_panel_closed(h.plugin);
	widget_down(&h);
}

/* Catches: init or the compositor (here, this test process) being
   signalled because an item id said so. */
static void
test_refuses_init_and_self(void)
{
	BarHarness h;
	g_autofree gchar *init_id = NULL;
	g_autofree gchar *self_id = NULL;

	if (!widget_up(&h))
		return;
	init_id = g_strdup_printf("sig:1:1:%u", getuid());
	self_id = g_strdup_printf("sig:%d:1:%u", (gint)getpid(), getuid());

	gowl_bar_plugin_panel_action(h.plugin, init_id, 0, 0.0, 1);
	g_assert_true(bar_harness_toasted(&h, "init or the compositor"));
	g_ptr_array_set_size(h.host->toasts, 0);

	/* Both steps of Force kill, against ourselves: if this did
	   anything the test would not be here to fail. */
	gowl_bar_plugin_panel_action(h.plugin, self_id, 1, 0.0, 1);
	gowl_bar_plugin_panel_action(h.plugin, self_id, 1, 0.0, 1);
	g_assert_true(bar_harness_toasted(&h, "init or the compositor"));
	widget_down(&h);
}

/* Catches: the journal written when off, not written when on, kept
   somewhere other than the host's state directory, and an export that
   never tells the user where it went. */
static void
test_journal_through_the_widget(void)
{
	BarHarness h;
	GowlBarPanel *panel;
	g_autofree gchar *path = NULL;
	g_autofree gchar *text = NULL;
	g_autofree gchar *export = NULL;
	gchar *(*export_path)(const gchar *, GDateTime *);

	if (!widget_up(&h))
		return;
	path = g_build_filename(h.host->state_dir, "activity.journal", NULL);

	bar_harness_poll(&h);
	g_assert_false(g_file_test(path, G_FILE_TEST_EXISTS));

	bar_harness_set(&h, "journal", "true");
	bar_harness_poll(&h);
	g_assert_true(g_file_get_contents(path, &text, NULL, NULL));
	g_assert_true(g_regex_match_simple("^[0-9]+,[0-9]+,[0-9]+,[0-9]*\n$",
	                                   text, 0, 0));

	panel = bar_harness_panel(&h);
	g_assert_nonnull(find_item(panel, "journal"));
	g_object_unref(panel);

	gowl_bar_plugin_panel_action(h.plugin, "journal", 0, 0.0, 1);
	panel = bar_harness_panel(&h);   /* raises the worker's toast */
	g_object_unref(panel);
	export_path = bar_harness_sym(&h, "bar_activity_export_path");
	export = export_path(test_home, NULL);
	g_assert_true(bar_harness_toasted(&h, "Activity journal exported"));
	g_assert_true(bar_harness_toasted(&h, export));
	g_assert_true(g_file_test(export, G_FILE_TEST_EXISTS));
	g_unlink(export);
	widget_down(&h);
}

/* Catches: the `activity' alias not resolving to the cpu widget. */
static void
test_activity_alias(void)
{
	BarHarness h;
	g_autoptr(GowlBarPlugin) plugin = NULL;
	GError *error = NULL;

	if (!widget_up(&h))
		return;
	plugin = gowl_bar_registry_instantiate(h.registry, "activity",
	                                       &error);
	g_assert_no_error(error);
	g_assert_nonnull(plugin);
	g_assert_cmpstr(gowl_bar_plugin_get_title(plugin), ==, "CPU");
	widget_down(&h);
}

int
main(int argc, char *argv[])
{
	gint rc;

	/* Exports land in $HOME; give this run its own, before anything
	   asks GLib for the home directory (it caches the answer). */
	test_home = g_build_filename(g_get_tmp_dir(),
	                             "gowl-cpu-activity-home-XXXXXX", NULL);
	g_assert_nonnull(g_mkdtemp(test_home));
	g_setenv("HOME", test_home, TRUE);

	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/bar-cpu-activity/settings/sampling",
	                test_sampling_speeds);
	g_test_add_func("/bar-cpu-activity/settings/temperature",
	                test_temperature_units);
	g_test_add_func("/bar-cpu-activity/settings/style-and-numbers",
	                test_style_and_numeric_settings);
	g_test_add_func("/bar-cpu-activity/creature/fill",
	                test_creature_fill_maths);
	g_test_add_func("/bar-cpu-activity/creature/geometry",
	                test_creature_geometry);
	g_test_add_func("/bar-cpu-activity/creature/wiggle",
	                test_creature_wiggle);
	g_test_add_func("/bar-cpu-activity/creature/colours",
	                test_creature_colour_settings);
	g_test_add_func("/bar-cpu-activity/stat/per-core",
	                test_per_core_parse_and_delta);
	g_test_add_func("/bar-cpu-activity/stat/frequencies",
	                test_frequency_parse);
	g_test_add_func("/bar-cpu-activity/stat/die",
	                test_die_bands_and_grid);
	g_test_add_func("/bar-cpu-activity/proc/stat", test_pid_stat_parse);
	g_test_add_func("/bar-cpu-activity/proc/status",
	                test_pid_status_parse);
	g_test_add_func("/bar-cpu-activity/proc/uid-filter",
	                test_read_processes_uid_filter);
	g_test_add_func("/bar-cpu-activity/proc/cpu-delta",
	                test_process_cpu_delta);
	g_test_add_func("/bar-cpu-activity/proc/filter", test_process_filter);
	g_test_add_func("/bar-cpu-activity/proc/kill-check", test_kill_check);
	g_test_add_func("/bar-cpu-activity/proc/item-id", test_proc_id_parse);
	g_test_add_func("/bar-cpu-activity/gpu/nvidia",
	                test_nvidia_argv_and_csv);
	g_test_add_func("/bar-cpu-activity/disk/mounts", test_mounts_parse);
	g_test_add_func("/bar-cpu-activity/journal/prune-age",
	                test_journal_prune_by_age);
	g_test_add_func("/bar-cpu-activity/journal/prune-size",
	                test_journal_prune_by_size);
	g_test_add_func("/bar-cpu-activity/journal/append-export",
	                test_journal_append_and_export);
	g_test_add_func("/bar-cpu-activity/widget/label-default",
	                test_label_style_unchanged_by_default);
	g_test_add_func("/bar-cpu-activity/widget/creature",
	                test_creature_style_draws);
	g_test_add_func("/bar-cpu-activity/widget/history",
	                test_history_samples);
	g_test_add_func("/bar-cpu-activity/widget/panel",
	                test_panel_compact_and_expanded);
	g_test_add_func("/bar-cpu-activity/widget/open-expanded",
	                test_open_expanded_setting);
	g_test_add_func("/bar-cpu-activity/widget/temperature-unit",
	                test_panel_temperature_unit);
	g_test_add_func("/bar-cpu-activity/widget/frequencies",
	                test_panel_frequencies);
	g_test_add_func("/bar-cpu-activity/widget/nvidia", test_panel_nvidia);
	g_test_add_func("/bar-cpu-activity/widget/terminate",
	                test_terminate_end_to_end);
	g_test_add_func("/bar-cpu-activity/widget/force-kill",
	                test_force_kill_two_step);
	g_test_add_func("/bar-cpu-activity/widget/refuse-init-self",
	                test_refuses_init_and_self);
	g_test_add_func("/bar-cpu-activity/widget/journal",
	                test_journal_through_the_widget);
	g_test_add_func("/bar-cpu-activity/widget/alias",
	                test_activity_alias);

	rc = g_test_run();
	{
		g_autofree gchar *cmd = g_strdup_printf("rm -rf '%s'",
		                                        test_home);
		if (system(cmd) != 0)
			g_printerr("could not remove %s\n", test_home);
	}
	g_free(test_home);
	return rc;
}
