/* test-pointer-confine.c -- compositor pointer confinement, the rules
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The decisions behind `toggle_pointer_confine' and the
 * `confine-pointer' window rule, without a compositor: the box the
 * pointer is held in, the clamp at every scale, what a focus change
 * does, what focus pinning refuses, when a workspace change lets go,
 * which key is the escape -- and the config that drives them.
 *
 * test-pointer-confine-live puts the same rules in front of a real
 * client on a headless compositor.
 */

#include <glib-object.h>
#include <glib/gstdio.h>
#include <string.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#include "core/gowl-pointer-confine.h"
#include "config/gowl-config.h"
#include "config/gowl-keybind.h"
#include "gowl-enums.h"

/* A box, filled in one line. */
static struct wlr_box
box(gint x, gint y, gint w, gint h)
{
	struct wlr_box b;

	b.x = x;
	b.y = y;
	b.width = w;
	b.height = h;
	return b;
}

#define assert_box(b, X, Y, W, H) G_STMT_START {          \
	g_assert_cmpint((b).x, ==, (X));                     \
	g_assert_cmpint((b).y, ==, (Y));                     \
	g_assert_cmpint((b).width, ==, (W));                 \
	g_assert_cmpint((b).height, ==, (H));                \
} G_STMT_END

/* ── the box ──────────────────────────────────────────────────────── */

/*
 * The pointer is held on the SURFACE, not the frame around it: a cursor
 * resting on the border is over no window, and a game reading its own
 * coordinates would see the pointer leave.
 */
static void
test_box_excludes_border(void)
{
	struct wlr_box geom = box(100, 50, 400, 300);
	struct wlr_box out;

	g_assert_true(gowl_pointer_confine_compute_box(&geom, 2, NULL, NULL,
	                                               &out));
	assert_box(out, 102, 52, 396, 296);

	/* Fullscreen windows have no border. */
	g_assert_true(gowl_pointer_confine_compute_box(&geom, 0, NULL, NULL,
	                                               &out));
	assert_box(out, 100, 50, 400, 300);
}

/*
 * A scrolling layout keeps the window's geometry wider than the screen
 * on purpose and draws only the part on it.  The pointer must not be
 * offered the part that is not drawn.
 */
static void
test_box_cut_to_drawn_frame(void)
{
	struct wlr_box geom = box(-300, 0, 1000, 600);   /* hangs off left */
	struct wlr_box frame = box(0, 0, 700, 600);      /* what is drawn */
	struct wlr_box out;

	g_assert_true(gowl_pointer_confine_compute_box(&geom, 1, &frame, NULL,
	                                               &out));
	assert_box(out, 1, 1, 698, 598);
}

/*
 * A frame that does not overlap the geometry at all is an animation in
 * flight, not a window with nothing drawn: the destination stands
 * rather than the confinement falling away for the length of the move.
 */
static void
test_box_disjoint_frame_keeps_geometry(void)
{
	struct wlr_box geom = box(2000, 0, 800, 600);
	struct wlr_box frame = box(0, 0, 800, 600);
	struct wlr_box out;

	g_assert_true(gowl_pointer_confine_compute_box(&geom, 0, &frame, NULL,
	                                               &out));
	assert_box(out, 2000, 0, 800, 600);
}

/*
 * Multi-monitor: a window hanging over the edge of its output into the
 * next one, or into a gap the layout has no pixels for, is cut to its
 * own output.  Outputs left of the origin have negative coordinates,
 * and the arithmetic must not care.
 */
static void
test_box_cut_to_output(void)
{
	struct wlr_box geom = box(-200, 900, 600, 400);
	struct wlr_box output = box(-1920, 0, 1920, 1080);
	struct wlr_box out;

	g_assert_true(gowl_pointer_confine_compute_box(&geom, 0, NULL, &output,
	                                               &out));
	assert_box(out, -200, 900, 200, 180);
}

/* A window entirely off its own output keeps its own box -- an empty
 * confinement would be no confinement at all. */
static void
test_box_off_output_keeps_own(void)
{
	struct wlr_box geom = box(3000, 0, 640, 480);
	struct wlr_box output = box(0, 0, 1920, 1080);
	struct wlr_box out;

	g_assert_true(gowl_pointer_confine_compute_box(&geom, 0, NULL, &output,
	                                               &out));
	assert_box(out, 3000, 0, 640, 480);
}

/* No area, no box: the caller treats that as nothing to confine to. */
static void
test_box_empty(void)
{
	struct wlr_box geom = box(0, 0, 4, 4);
	struct wlr_box out;

	g_assert_false(gowl_pointer_confine_compute_box(&geom, 2, NULL, NULL,
	                                                &out));
	geom = box(0, 0, 0, 100);
	g_assert_false(gowl_pointer_confine_compute_box(&geom, 0, NULL, NULL,
	                                                &out));
}

/* ── the clamp ────────────────────────────────────────────────────── */

/* Inside is left alone, exactly -- a clamp that nudged a position it
 * did not need to would drift the pointer on every event. */
static void
test_clamp_inside_untouched(void)
{
	struct wlr_box b = box(100, 100, 400, 300);
	gdouble x = 250.75, y = 101.25;

	g_assert_false(gowl_pointer_confine_clamp(&b, 1.0, &x, &y));
	g_assert_cmpfloat(x, ==, 250.75);
	g_assert_cmpfloat(y, ==, 101.25);
	g_assert_true(gowl_pointer_confine_contains(&b, 1.0, 250.75, 101.25));
}

/* At scale 1 the last position is one pixel short of the far edge:
 * the window's last column, never the first of its neighbour. */
static void
test_clamp_scale_one(void)
{
	struct wlr_box b = box(100, 100, 400, 300);
	gdouble x = 9000.0, y = -50.0;

	g_assert_true(gowl_pointer_confine_clamp(&b, 1.0, &x, &y));
	g_assert_cmpfloat(x, ==, 499.0);
	g_assert_cmpfloat(y, ==, 100.0);
	g_assert_false(gowl_pointer_confine_contains(&b, 1.0, 500.0, 200.0));
	g_assert_true(gowl_pointer_confine_contains(&b, 1.0, 499.0, 399.0));
}

/*
 * HiDPI: one PHYSICAL pixel short of the edge.  At scale 2 that is half
 * a logical pixel, so the right-most physical column -- the one an
 * edge-scrolling game watches -- is still reachable; at 1.5 it is two
 * thirds of one.  Clamping to a whole logical pixel would have left the
 * last physical column unreachable at every scale above 1.
 */
static void
test_clamp_hidpi(void)
{
	struct wlr_box b = box(0, 0, 1280, 720);
	gdouble x, y;

	x = 5000.0;
	y = 5000.0;
	g_assert_true(gowl_pointer_confine_clamp(&b, 2.0, &x, &y));
	g_assert_cmpfloat(x, ==, 1279.5);
	g_assert_cmpfloat(y, ==, 719.5);

	x = 5000.0;
	y = 5000.0;
	g_assert_true(gowl_pointer_confine_clamp(&b, 1.5, &x, &y));
	g_assert_cmpfloat_with_epsilon(x, 1280.0 - 1.0 / 1.5, 1e-9);
	g_assert_cmpfloat_with_epsilon(y, 720.0 - 1.0 / 1.5, 1e-9);

	/* A scale nobody set is treated as 1, not divided by. */
	x = 5000.0;
	y = 5000.0;
	g_assert_true(gowl_pointer_confine_clamp(&b, 0.0, &x, &y));
	g_assert_cmpfloat(x, ==, 1279.0);
}

/* Multi-monitor: a box on an output left of and above the origin. */
static void
test_clamp_negative_layout(void)
{
	struct wlr_box b = box(-1920, -1080, 1920, 1080);
	gdouble x = 10.0, y = -2000.0;

	g_assert_true(gowl_pointer_confine_clamp(&b, 1.0, &x, &y));
	g_assert_cmpfloat(x, ==, -1.0);
	g_assert_cmpfloat(y, ==, -1080.0);
}

/* A box narrower than a physical pixel still has somewhere to put the
 * pointer: its origin, rather than a maximum below its minimum. */
static void
test_clamp_tiny_box(void)
{
	struct wlr_box b = box(10, 10, 1, 1);
	gdouble x = 50.0, y = 0.0;

	g_assert_true(gowl_pointer_confine_clamp(&b, 0.5, &x, &y));
	g_assert_cmpfloat(x, ==, 10.0);
	g_assert_cmpfloat(y, ==, 10.0);
}

/* ── focus ────────────────────────────────────────────────────────── */

/* Each row: confined, target_null, same, unmanaged, ruled, suppressed,
 * and what must happen. */
typedef struct {
	gboolean confined, target_null, same, unmanaged, ruled, suppressed;
	GowlConfineFocusStep want;
	const gchar *why;
} FocusRow;

static const FocusRow focus_rows[] = {
	{ FALSE, FALSE, FALSE, FALSE, FALSE, FALSE, GOWL_CONFINE_FOCUS_KEEP,
	  "nothing confined, ordinary window: nothing to do" },
	{ TRUE,  FALSE, FALSE, FALSE, FALSE, FALSE, GOWL_CONFINE_FOCUS_RELEASE,
	  "focus moved to another window: let go" },
	{ TRUE,  TRUE,  FALSE, FALSE, FALSE, FALSE, GOWL_CONFINE_FOCUS_RELEASE,
	  "focus cleared (an empty tag): let go" },
	{ TRUE,  FALSE, TRUE,  FALSE, FALSE, FALSE, GOWL_CONFINE_FOCUS_KEEP,
	  "focus back on the confined window: keep" },
	{ TRUE,  FALSE, TRUE,  FALSE, TRUE,  FALSE, GOWL_CONFINE_FOCUS_KEEP,
	  "the ruled window refocused: keep, do not restart" },
	{ TRUE,  FALSE, FALSE, TRUE,  FALSE, FALSE, GOWL_CONFINE_FOCUS_KEEP,
	  "the game's own popup took the keyboard: keep" },
	{ FALSE, FALSE, FALSE, TRUE,  TRUE,  FALSE, GOWL_CONFINE_FOCUS_KEEP,
	  "a popup never starts one, ruled or not" },
	{ FALSE, FALSE, FALSE, FALSE, TRUE,  FALSE, GOWL_CONFINE_FOCUS_CONFINE,
	  "a ruled window took focus: confine to it" },
	{ TRUE,  FALSE, FALSE, FALSE, TRUE,  FALSE, GOWL_CONFINE_FOCUS_CONFINE,
	  "from one ruled game to another: move the confinement" },
	{ FALSE, FALSE, FALSE, FALSE, TRUE,  TRUE,  GOWL_CONFINE_FOCUS_KEEP,
	  "a ruled window the user just let go of: the rule waits" },
	{ FALSE, TRUE,  FALSE, FALSE, TRUE,  FALSE, GOWL_CONFINE_FOCUS_KEEP,
	  "no window cannot be confined to, whatever the flags say" },
};

static void
test_focus_steps(void)
{
	gsize i;

	for (i = 0; i < G_N_ELEMENTS(focus_rows); i++) {
		const FocusRow *r = &focus_rows[i];
		GowlConfineFocusStep got;

		got = gowl_pointer_confine_focus_step(r->confined, r->target_null,
		                                      r->same, r->unmanaged,
		                                      r->ruled, r->suppressed);
		if (got != r->want)
			g_error("focus row %" G_GSIZE_FORMAT " (%s): got %d, "
			        "want %d", i, r->why, (gint)got, (gint)r->want);
	}
}

/*
 * Focus pinning refuses the automatic focus changes -- not the confined
 * window itself, and not the confined program's own windows: a game's
 * dialog is the game talking, not another program stealing the stream.
 */
static void
test_pin_blocks(void)
{
	g_assert_false(gowl_pointer_confine_pin_blocks(FALSE, FALSE, FALSE));
	g_assert_true(gowl_pointer_confine_pin_blocks(TRUE, FALSE, FALSE));
	g_assert_false(gowl_pointer_confine_pin_blocks(TRUE, TRUE, FALSE));
	g_assert_false(gowl_pointer_confine_pin_blocks(TRUE, FALSE, TRUE));
}

/* ── workspace ────────────────────────────────────────────────────── */

/* Switching workspace lets go even when the window is still visible --
 * a sticky one is -- because the user is doing something else now. */
static void
test_view_lost(void)
{
	g_assert_false(gowl_pointer_confine_view_lost(1, 1, TRUE));
	g_assert_true(gowl_pointer_confine_view_lost(1, 2, FALSE));
	g_assert_true(gowl_pointer_confine_view_lost(1, 2, TRUE));
	g_assert_true(gowl_pointer_confine_view_lost(1, 1, FALSE));
	/* Viewing a second tag beside the first is a change too. */
	g_assert_true(gowl_pointer_confine_view_lost(1, 3, TRUE));
}

/* ── the escape key ───────────────────────────────────────────────── */

/* The shipped escape parses, and matches only itself: Escape with one
 * modifier more or less is somebody else's chord. */
static void
test_escape_default(void)
{
	guint mods = 0, sym = 0;

	g_assert_true(gowl_keybind_parse(GOWL_POINTER_CONFINE_DEFAULT_ESCAPE,
	                                 &mods, &sym));
	g_assert_cmpuint(sym, ==, XKB_KEY_Escape);
	g_assert_cmpuint(mods, ==, GOWL_KEY_MOD_LOGO | GOWL_KEY_MOD_CTRL);

	g_assert_true(gowl_pointer_confine_escape_matches(mods, sym, mods,
	                                                  XKB_KEY_Escape));
	g_assert_false(gowl_pointer_confine_escape_matches(
		mods, sym, GOWL_KEY_MOD_LOGO, XKB_KEY_Escape));
	g_assert_false(gowl_pointer_confine_escape_matches(
		mods, sym, mods | GOWL_KEY_MOD_SHIFT, XKB_KEY_Escape));
	g_assert_false(gowl_pointer_confine_escape_matches(mods, sym, mods,
	                                                   XKB_KEY_q));
	/* A keysym of 0 -- an unparsable key -- matches nothing at all. */
	g_assert_false(gowl_pointer_confine_escape_matches(0, 0, 0, 0));
}

/* ── config ───────────────────────────────────────────────────────── */

/* Load @yaml into @cfg the way gowl does: from a file. */
static gboolean
load_string(GowlConfig *cfg, const gchar *yaml, GError **error)
{
	g_autofree gchar *dir = NULL;
	g_autofree gchar *path = NULL;
	gboolean ok;

	dir = g_dir_make_tmp("gowl-confine-XXXXXX", error);
	if (dir == NULL)
		return FALSE;
	path = g_build_filename(dir, "config.yaml", NULL);
	ok = g_file_set_contents(path, yaml, -1, error)
	     && gowl_config_load_yaml(cfg, path, error);
	g_unlink(path);
	g_rmdir(dir);
	return ok;
}

static GowlConfig *
config_from(const gchar *yaml)
{
	GowlConfig *cfg = gowl_config_new();
	GError *error = NULL;

	g_assert_true(load_string(cfg, yaml, &error));
	g_assert_no_error(error);
	return cfg;
}

/* The defaults: focus pinned, Super+Ctrl+Escape. */
static void
test_config_defaults(void)
{
	g_autoptr(GowlConfig) cfg = gowl_config_new();

	g_assert_true(gowl_config_get_pointer_confine_focus_pin(cfg));
	g_assert_cmpstr(gowl_config_get_pointer_confine_escape(cfg), ==,
	                "Super+Ctrl+Escape");
}

static void
test_config_settings(void)
{
	g_autoptr(GowlConfig) cfg = NULL;
	g_autofree gchar *out = NULL;

	cfg = config_from("pointer-confine-focus-pin: false\n"
	                  "pointer-confine-escape: \"Super+Alt+Escape\"\n");
	g_assert_false(gowl_config_get_pointer_confine_focus_pin(cfg));
	g_assert_cmpstr(gowl_config_get_pointer_confine_escape(cfg), ==,
	                "Super+Alt+Escape");

	/* And they survive a round trip through the YAML writer. */
	out = gowl_config_generate_yaml(cfg);
	g_assert_nonnull(strstr(out, "pointer-confine-focus-pin: false"));
	g_assert_nonnull(strstr(out,
		"pointer-confine-escape: \"Super+Alt+Escape\""));
}

/*
 * The build defines G_LOG_USE_STRUCTURED, so a warning goes through the
 * log writer and g_test_expect_message() never sees it -- GTest's abort
 * on warnings fires instead.  While `swallowing' is set this writer
 * eats warnings and remembers whether the one wanted came by; the rest
 * of the time it passes everything to the default writer.  A writer
 * can be installed only once per process, so it stays.
 */
static gboolean swallowing = FALSE;
static gboolean saw_escape_warning = FALSE;

static GLogWriterOutput
swallow_writer(GLogLevelFlags   level,
               const GLogField *fields,
               gsize            n_fields,
               gpointer         user_data)
{
	gsize i;

	if (swallowing && (level & G_LOG_LEVEL_WARNING) != 0) {
		for (i = 0; i < n_fields; i++) {
			if (g_strcmp0(fields[i].key, "MESSAGE") == 0
			    && fields[i].length < 0
			    && strstr((const gchar *)fields[i].value,
			              "pointer-confine-escape") != NULL)
				saw_escape_warning = TRUE;
		}
		return G_LOG_WRITER_HANDLED;
	}
	return g_log_writer_default(level, fields, n_fields, user_data);
}

/*
 * An escape that does not parse must never leave the user with no
 * escape: it is a reported problem and the default stays.
 */
static void
test_config_bad_escape_keeps_default(void)
{
	g_autoptr(GowlConfig) cfg = gowl_config_new();
	GError *error = NULL;

	g_log_set_writer_func(swallow_writer, NULL, NULL);
	swallowing = TRUE;
	saw_escape_warning = FALSE;
	load_string(cfg, "pointer-confine-escape: \"Super+NotAKey\"\n",
	            &error);
	swallowing = FALSE;
	g_clear_error(&error);

	g_assert_true(saw_escape_warning);
	g_assert_cmpstr(gowl_config_get_pointer_confine_escape(cfg), ==,
	                "Super+Ctrl+Escape");
}

/* The window rule: `confine-pointer' parses, matches by app-id like any
 * rule (an X11 window's app-id is its class), and is written back. */
static void
test_config_rule(void)
{
	g_autoptr(GowlConfig) cfg = NULL;
	g_autofree gchar *out = NULL;
	GPtrArray *rules;
	GowlRuleEntry *r;

	cfg = config_from("rules:\n"
	                  "  - app-id: \"wow.exe\"\n"
	                  "    confine-pointer: true\n"
	                  "  - app-id: \"firefox\"\n"
	                  "    tags: 2\n");
	rules = gowl_config_get_rules(cfg);
	g_assert_cmpuint(rules->len, ==, 2);

	r = g_ptr_array_index(rules, 0);
	g_assert_cmpstr(r->app_id, ==, "wow.exe");
	g_assert_true(r->confine_pointer);
	r = g_ptr_array_index(rules, 1);
	g_assert_false(r->confine_pointer);

	out = gowl_config_generate_yaml(cfg);
	g_assert_nonnull(strstr(out, "confine-pointer: true"));
}

/* The action is reachable by name the way config, IPC and cmacs spell
 * it: the nick, with `_' read as `-'. */
static void
test_action_nick(void)
{
	GEnumClass *klass = g_type_class_ref(GOWL_TYPE_ACTION);
	GEnumValue *v;

	v = g_enum_get_value_by_nick(klass, "toggle-pointer-confine");
	g_assert_nonnull(v);
	g_assert_cmpint(v->value, ==, GOWL_ACTION_TOGGLE_POINTER_CONFINE);
	g_type_class_unref(klass);
}

int
main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/pointer-confine/box/border", test_box_excludes_border);
	g_test_add_func("/pointer-confine/box/drawn-frame",
	                test_box_cut_to_drawn_frame);
	g_test_add_func("/pointer-confine/box/disjoint-frame",
	                test_box_disjoint_frame_keeps_geometry);
	g_test_add_func("/pointer-confine/box/output", test_box_cut_to_output);
	g_test_add_func("/pointer-confine/box/off-output",
	                test_box_off_output_keeps_own);
	g_test_add_func("/pointer-confine/box/empty", test_box_empty);
	g_test_add_func("/pointer-confine/clamp/inside",
	                test_clamp_inside_untouched);
	g_test_add_func("/pointer-confine/clamp/scale-1", test_clamp_scale_one);
	g_test_add_func("/pointer-confine/clamp/hidpi", test_clamp_hidpi);
	g_test_add_func("/pointer-confine/clamp/negative-layout",
	                test_clamp_negative_layout);
	g_test_add_func("/pointer-confine/clamp/tiny", test_clamp_tiny_box);
	g_test_add_func("/pointer-confine/focus/steps", test_focus_steps);
	g_test_add_func("/pointer-confine/focus/pin", test_pin_blocks);
	g_test_add_func("/pointer-confine/view-lost", test_view_lost);
	g_test_add_func("/pointer-confine/escape/default", test_escape_default);
	g_test_add_func("/pointer-confine/config/defaults", test_config_defaults);
	g_test_add_func("/pointer-confine/config/settings", test_config_settings);
	g_test_add_func("/pointer-confine/config/bad-escape",
	                test_config_bad_escape_keeps_default);
	g_test_add_func("/pointer-confine/config/rule", test_config_rule);
	g_test_add_func("/pointer-confine/action-nick", test_action_nick);

	return g_test_run();
}
