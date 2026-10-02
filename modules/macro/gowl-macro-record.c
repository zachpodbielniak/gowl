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
 * gowl-macro-record.c - Turn a recording of real input into a macro.
 *
 * The recorder hands over raw events: every key press and release,
 * every pointer move, button and scroll, with an offset from the start.
 * A macro wants fewer, fatter steps, and three kinds of noise have to
 * go before the rest can be replayed:
 *
 *   - The record key itself.  Recording starts while Super+Alt+r is
 *     held, so the trace begins with the release of `r' and of the two
 *     modifiers, and ends with the press of the same chord to stop it.
 *     Those are exactly the events with no partner: a release whose
 *     press came before the recording, and a press never released
 *     before it ended.  Unpaired events are dropped, which removes the
 *     record key without the translator needing to know what it is.
 *   - Lone modifiers.  A key press carries the modifier mask in force,
 *     so `Ctrl+c' is one step, `gowl_macro_key_code(ctx, NULL, 46,
 *     CTRL)'.  The Control press and release around it would only
 *     replay as a stray tap of Control.
 *   - Pointer jitter.  Moves matter only where something happens: the
 *     position before a click or a scroll, and the path of a drag
 *     (sampled, not every event).  A wander across the screen between
 *     clicks is one move, to where the next click lands.
 *
 * What is left is written as a timeline macro, with the recorded
 * pauses between steps scaled by an optional first argument --
 * `macro-run NAME 2' replays at double speed.
 */

#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "gowl-macro"

#include <json-glib/json-glib.h>
#include <string.h>

#include "gowl-macro-record.h"

/* Modifier bits, as the recorder reports them (wlroots' mask). */
#define REC_MOD_SHIFT (1u << 0)
#define REC_MOD_CTRL  (1u << 2)
#define REC_MOD_ALT   (1u << 3)
#define REC_MOD_LOGO  (1u << 6)
#define REC_MOD_MOD5  (1u << 7)
/* The ones worth replaying: not Caps Lock, not Num Lock. */
#define REC_MOD_KEEP  (REC_MOD_SHIFT | REC_MOD_CTRL | REC_MOD_ALT \
                       | REC_MOD_LOGO | REC_MOD_MOD5)

/* BTN_LEFT, BTN_RIGHT, BTN_MIDDLE: named in the output for reading. */
#define REC_BTN_LEFT   (0x110)
#define REC_BTN_RIGHT  (0x111)
#define REC_BTN_MIDDLE (0x112)

/* A drag is sampled no finer than this; smoother is not more correct. */
#define REC_DRAG_SAMPLE_MS (16.0)
/* A pause shorter than this is not written: it is not a pause. */
#define REC_MIN_WAIT_MS (8)

typedef enum {
	ACT_KEY,        /* a tap: keycode + mods */
	ACT_CLICK,      /* press and release with no move between */
	ACT_BUTTON,     /* half a drag: press or release */
	ACT_POINTER,
	ACT_SCROLL
} ActKind;

typedef struct {
	ActKind   kind;
	gdouble   at_ms;
	gboolean  valid;      /* FALSE until its partner turns up */
	guint     keycode;
	guint     mods;
	gchar    *keysym;
	guint     button;
	gboolean  pressed;
	gdouble   x;
	gdouble   y;
	gboolean  horizontal;
	gdouble   value;
	gint      discrete;
} Act;

G_DEFINE_QUARK(gowl-macro-record-error-quark, gowl_macro_record_error)

static void
act_free(
	gpointer data
){
	Act *a = data;

	g_free(a->keysym);
	g_free(a);
}

static Act *
act_new(
	GPtrArray *acts,
	ActKind    kind,
	gdouble    at_ms
){
	Act *a;

	a = g_new0(Act, 1);
	a->kind = kind;
	a->at_ms = at_ms;
	a->valid = TRUE;
	g_ptr_array_add(acts, a);
	return a;
}

/* Whether @name is a modifier's keysym (the press is folded into mods). */
static gboolean
is_modifier_keysym(
	const gchar *name
){
	static const gchar *const mods[] = {
		"Shift_L", "Shift_R", "Control_L", "Control_R", "Alt_L",
		"Alt_R", "Meta_L", "Meta_R", "Super_L", "Super_R", "Hyper_L",
		"Hyper_R", "ISO_Level3_Shift", "ISO_Level5_Shift", "Caps_Lock",
		"Num_Lock", "Mode_switch", NULL
	};

	return name != NULL && g_strv_contains(mods, name);
}

/*
 * add_move:
 *
 * A pointer move to (@x, @y), unless the pointer is already there as
 * far as the macro knows.  @have/@cx/@cy track the position the last
 * written move left it at.
 */
static void
add_move(
	GPtrArray *acts,
	gdouble    at_ms,
	gdouble    x,
	gdouble    y,
	gboolean  *have,
	gdouble   *cx,
	gdouble   *cy
){
	Act *a;

	if (*have && (gint)*cx == (gint)x && (gint)*cy == (gint)y)
		return;
	a = act_new(acts, ACT_POINTER, at_ms);
	a->x = x;
	a->y = y;
	*have = TRUE;
	*cx = x;
	*cy = y;
}

/*
 * collect:
 *
 * One pass over the events, building the step list.  Presses are added
 * as invalid and validated by their release; whatever is still invalid
 * at the end was never released (the stop key) and is not written.
 */
static GPtrArray *
collect(
	JsonArray *events,
	guint     *dropped
){
	GPtrArray *acts;
	GHashTable *keys;      /* keycode -> Act* awaiting its release */
	GHashTable *buttons;   /* button  -> Act* awaiting its release */
	gboolean have_pos;
	gdouble cx;
	gdouble cy;
	gdouble px;            /* the pointer as last reported */
	gdouble py;
	gboolean have_px;
	gdouble last_drag_ms;
	guint i;
	guint n;

	acts = g_ptr_array_new_with_free_func(act_free);
	keys = g_hash_table_new(g_direct_hash, g_direct_equal);
	buttons = g_hash_table_new(g_direct_hash, g_direct_equal);
	have_pos = FALSE;
	have_px = FALSE;
	cx = cy = px = py = 0.0;
	last_drag_ms = -1e9;

	n = json_array_get_length(events);
	for (i = 0; i < n; i++) {
		JsonObject *e;
		const gchar *type;
		const gchar *state;
		gdouble at;
		gboolean press;

		e = json_array_get_object_element(events, i);
		if (e == NULL)
			continue;
		type = json_object_get_string_member_with_default(e, "type", "");
		at = json_object_get_double_member_with_default(e, "offset_ms", 0.0);
		state = json_object_get_string_member_with_default(e, "state", "");
		press = g_strcmp0(state, "press") == 0;

		/* --- keys: taps, validated by their release --- */
		if (g_strcmp0(type, "key") == 0) {
			guint code;
			const gchar *sym;
			Act *a;

			code = (guint)json_object_get_int_member_with_default(e,
				"keycode", 0);
			sym = json_object_get_string_member_with_default(e,
				"keysym", NULL);
			if (code == 0 || is_modifier_keysym(sym)) {
				(*dropped)++;
				continue;
			}
			if (press) {
				/* A repeat press of a held key is the same tap. */
				if (g_hash_table_contains(keys, GUINT_TO_POINTER(code)))
					continue;
				a = act_new(acts, ACT_KEY, at);
				a->valid = FALSE;
				a->keycode = code;
				a->keysym = g_strdup(sym);
				a->mods = (guint)json_object_get_int_member_with_default(
					e, "mods", 0) & REC_MOD_KEEP;
				g_hash_table_insert(keys, GUINT_TO_POINTER(code), a);
			} else {
				a = g_hash_table_lookup(keys, GUINT_TO_POINTER(code));
				if (a == NULL) {
					(*dropped)++;   /* pressed before we started */
					continue;
				}
				a->valid = TRUE;
				g_hash_table_remove(keys, GUINT_TO_POINTER(code));
			}
			continue;
		}

		/* --- pointer motion: only the last position matters, except
		       during a drag, where the path is sampled --- */
		if (g_strcmp0(type, "pointer_motion") == 0) {
			px = json_object_get_double_member_with_default(e, "x", 0.0);
			py = json_object_get_double_member_with_default(e, "y", 0.0);
			have_px = TRUE;
			if (g_hash_table_size(buttons) > 0
			    && at - last_drag_ms >= REC_DRAG_SAMPLE_MS) {
				add_move(acts, at, px, py, &have_pos, &cx, &cy);
				last_drag_ms = at;
			}
			continue;
		}

		/* --- buttons: a click, or the two halves of a drag --- */
		if (g_strcmp0(type, "pointer_button") == 0) {
			guint button;
			gdouble x;
			gdouble y;
			Act *a;

			button = (guint)json_object_get_int_member_with_default(e,
				"button", 0);
			x = json_object_get_double_member_with_default(e, "x", px);
			y = json_object_get_double_member_with_default(e, "y", py);
			if (press) {
				if (g_hash_table_contains(buttons,
				                          GUINT_TO_POINTER(button)))
					continue;
				add_move(acts, at, x, y, &have_pos, &cx, &cy);
				a = act_new(acts, ACT_BUTTON, at);
				a->valid = FALSE;
				a->button = button;
				a->pressed = TRUE;
				g_hash_table_insert(buttons, GUINT_TO_POINTER(button), a);
				last_drag_ms = at;
				continue;
			}
			a = g_hash_table_lookup(buttons, GUINT_TO_POINTER(button));
			if (a == NULL) {
				(*dropped)++;
				continue;
			}
			g_hash_table_remove(buttons, GUINT_TO_POINTER(button));
			if (acts->len > 0 && g_ptr_array_index(acts, acts->len - 1) == a
			    && (gint)x == (gint)cx && (gint)y == (gint)cy) {
				/* Nothing between the press and the release, and
				   the pointer did not move: a click. */
				a->kind = ACT_CLICK;
				a->valid = TRUE;
				continue;
			}
			add_move(acts, at, x, y, &have_pos, &cx, &cy);
			a->valid = TRUE;
			a = act_new(acts, ACT_BUTTON, at);
			a->button = button;
			a->pressed = FALSE;
			continue;
		}

		/* --- scrolls, where the pointer was --- */
		if (g_strcmp0(type, "pointer_axis") == 0) {
			Act *a;

			add_move(acts, at,
			         json_object_get_double_member_with_default(e, "x", px),
			         json_object_get_double_member_with_default(e, "y", py),
			         &have_pos, &cx, &cy);
			a = act_new(acts, ACT_SCROLL, at);
			a->horizontal = g_strcmp0(
				json_object_get_string_member_with_default(e, "axis", ""),
				"horizontal") == 0;
			a->value = json_object_get_double_member_with_default(e,
				"value", 0.0);
			a->discrete = (gint)json_object_get_int_member_with_default(e,
				"discrete", 0);
			continue;
		}
		/* `modifiers' events: already folded into each key's mods. */
	}

	/* The pointer's final resting place, when it moved after the last
	   thing that happened: a recording that ends by pointing somewhere
	   should leave the pointer there. */
	if (have_px && acts->len > 0)
		add_move(acts,
		         ((Act *)g_ptr_array_index(acts, acts->len - 1))->at_ms,
		         px, py, &have_pos, &cx, &cy);

	*dropped += g_hash_table_size(keys) + g_hash_table_size(buttons);
	g_hash_table_unref(keys);
	g_hash_table_unref(buttons);
	return acts;
}

static const gchar *
button_name(
	guint button
){
	switch (button) {
	case REC_BTN_LEFT:   return "BTN_LEFT";
	case REC_BTN_RIGHT:  return "BTN_RIGHT";
	case REC_BTN_MIDDLE: return "BTN_MIDDLE";
	default:             return NULL;
	}
}

/* The C spelling of a modifier mask, `GOWL_KEY_MOD_CTRL | ...' or `0'. */
static gchar *
mods_text(
	guint mods
){
	GString *out;

	if (mods == 0)
		return g_strdup("0");
	out = g_string_new(NULL);
	if (mods & REC_MOD_LOGO)
		g_string_append(out, " | GOWL_KEY_MOD_LOGO");
	if (mods & REC_MOD_CTRL)
		g_string_append(out, " | GOWL_KEY_MOD_CTRL");
	if (mods & REC_MOD_ALT)
		g_string_append(out, " | GOWL_KEY_MOD_ALT");
	if (mods & REC_MOD_SHIFT)
		g_string_append(out, " | GOWL_KEY_MOD_SHIFT");
	if (mods & REC_MOD_MOD5)
		g_string_append(out, " | GOWL_KEY_MOD_MOD5");
	g_string_erase(out, 0, 3);
	return g_string_free(out, FALSE);
}

/* Whether @c can go in a C comment as-is. */
static gboolean
comment_safe(
	const gchar *text
){
	const gchar *p;

	if (text == NULL)
		return FALSE;
	for (p = text; *p != '\0'; p++)
		if (!g_ascii_isalnum(*p) && *p != '_')
			return FALSE;
	return TRUE;
}

/**
 * gowl_macro_record_to_source:
 * @payload: the recorder's JSON: an object with an `events' array, as
 *   gowl_input_recorder_drain() or _stop() return
 * @name: the macro's name, for its description
 * @max_gap_ms: a recorded pause longer than this is shortened to it
 *   (somebody thinking is not part of the procedure); 0 keeps them all
 * @stats: (out caller-allocates) (optional): what was made of it
 * @error: return location for a parse error, or for a recording with
 *   nothing in it worth replaying
 *
 * Writes the C source of a timeline macro that replays the recording.
 * See the file comment for what is kept and what is left out.
 *
 * Returns: (transfer full) (nullable): the source, or %NULL on error
 */
gchar *
gowl_macro_record_to_source(
	const gchar          *payload,
	const gchar          *name,
	guint                 max_gap_ms,
	GowlMacroRecordStats *stats,
	GError              **error
){
	g_autoptr(JsonParser) parser = NULL;
	g_autoptr(GPtrArray) acts = NULL;
	g_autoptr(GDateTime) now = NULL;
	g_autofree gchar *stamp = NULL;
	GowlMacroRecordStats st;
	JsonNode *root;
	JsonObject *obj;
	JsonArray *events;
	GString *body;
	GString *src;
	gdouble prev_ms;
	gboolean first;
	guint i;

	g_return_val_if_fail(payload != NULL, NULL);

	memset(&st, 0, sizeof st);
	parser = json_parser_new();
	if (!json_parser_load_from_data(parser, payload, -1, error))
		return NULL;
	root = json_parser_get_root(parser);
	if (root == NULL || !JSON_NODE_HOLDS_OBJECT(root)) {
		g_set_error_literal(error, GOWL_MACRO_RECORD_ERROR,
		                    GOWL_MACRO_RECORD_ERROR_PARSE,
		                    "the recording is not a JSON object");
		return NULL;
	}
	obj = json_node_get_object(root);
	events = json_object_has_member(obj, "events")
		? json_object_get_array_member(obj, "events") : NULL;
	if (events == NULL) {
		g_set_error_literal(error, GOWL_MACRO_RECORD_ERROR,
		                    GOWL_MACRO_RECORD_ERROR_PARSE,
		                    "the recording has no events array");
		return NULL;
	}
	st.suppressed = (guint)json_object_get_int_member_with_default(obj,
		"suppressed_total", 0);

	acts = collect(events, &st.dropped);

	/* --- the body: one line per step, a wait before each one that
	       came a noticeable while after the last --- */
	body = g_string_new(NULL);
	prev_ms = 0.0;
	first = TRUE;
	for (i = 0; i < acts->len; i++) {
		Act *a = g_ptr_array_index(acts, i);
		guint gap;

		if (!a->valid)
			continue;
		gap = first ? 0 : (guint)MAX(a->at_ms - prev_ms, 0.0);
		if (max_gap_ms > 0 && gap > max_gap_ms)
			gap = max_gap_ms;
		if (gap >= REC_MIN_WAIT_MS) {
			g_string_append_printf(body,
				"\tgowl_macro_wait(ctx, scaled(ctx, %u));\n", gap);
			st.duration_ms += gap;
		}
		prev_ms = a->at_ms;
		first = FALSE;
		st.steps++;

		switch (a->kind) {
		case ACT_KEY: {
			g_autofree gchar *m = mods_text(a->mods);

			if (comment_safe(a->keysym))
				g_string_append_printf(body,
					"\tgowl_macro_key_code(ctx, NULL, %u, %s);"
					"\t/* %s */\n", a->keycode, m, a->keysym);
			else
				g_string_append_printf(body,
					"\tgowl_macro_key_code(ctx, NULL, %u, %s);\n",
					a->keycode, m);
			break;
		}
		case ACT_CLICK:
			if (button_name(a->button) != NULL)
				g_string_append_printf(body,
					"\tgowl_macro_button(ctx, %s);\n",
					button_name(a->button));
			else
				g_string_append_printf(body,
					"\tgowl_macro_button(ctx, %u);\n", a->button);
			break;
		case ACT_BUTTON:
			if (button_name(a->button) != NULL)
				g_string_append_printf(body,
					"\tgowl_macro_button_state(ctx, %s, %s);\n",
					button_name(a->button),
					a->pressed ? "TRUE" : "FALSE");
			else
				g_string_append_printf(body,
					"\tgowl_macro_button_state(ctx, %u, %s);\n",
					a->button, a->pressed ? "TRUE" : "FALSE");
			break;
		case ACT_POINTER:
			g_string_append_printf(body,
				"\tgowl_macro_pointer(ctx, %d, %d);\n",
				(gint)a->x, (gint)a->y);
			break;
		case ACT_SCROLL: {
			gchar value[G_ASCII_DTOSTR_BUF_SIZE];

			/* g_ascii_dtostr: a comma decimal point in a C file
			   is a syntax error, and the user's locale may have one */
			g_ascii_dtostr(value, sizeof value, a->value);
			g_string_append_printf(body,
				"\tgowl_macro_scroll(ctx, %s, %s, %d);\n",
				a->horizontal ? "TRUE" : "FALSE", value, a->discrete);
			break;
		}
		}
	}

	if (st.steps == 0) {
		g_string_free(body, TRUE);
		g_set_error_literal(error, GOWL_MACRO_RECORD_ERROR,
		                    GOWL_MACRO_RECORD_ERROR_EMPTY,
		                    st.suppressed > 0
		                    ? "nothing was recorded: everything typed "
		                      "went to a window the recorder protects "
		                      "(a password prompt or the lock screen)"
		                    : "nothing was recorded");
		return NULL;
	}

	/* --- the file around it --- */
	now = g_date_time_new_now_local();
	stamp = g_date_time_format(now, "%Y-%m-%d %H:%M");
	src = g_string_new(NULL);
	g_string_append_printf(src,
		"/*\n"
		" * %s.c - recorded by the gowl macro recorder, %s.\n"
		" *\n"
		" * %u step(s), %u.%u s at normal speed.  An optional first\n"
		" * argument is a speed factor: `macro-run %s 2' plays it twice\n"
		" * as fast, `0.5' at half speed.\n"
		" *\n"
		" * Keys replay into whatever window has focus when they play, and\n"
		" * the pointer moves to the same screen positions, so a\n"
		" * recording is only as portable as the layout it was made on.\n"
		" * It is ordinary C: edit it, rename it, bind it to a key.\n",
		name, stamp, st.steps, st.duration_ms / 1000,
		(st.duration_ms % 1000) / 100, name);
	if (st.suppressed > 0)
		g_string_append_printf(src,
			" *\n"
			" * %u event(s) were NOT recorded, because they went to a\n"
			" * window the recorder protects (a password prompt, the\n"
			" * lock screen).  The replay will be missing that input.\n",
			st.suppressed);
	g_string_append_printf(src,
		" */\n"
		"\n"
		"#include <gowl/gowl.h>\n"
		"\n"
		"G_MODULE_EXPORT const gchar *\n"
		"gowl_macro_info(void)\n"
		"{\n"
		"\treturn \"Recorded %s: %u steps, %u.%u s\";\n"
		"}\n"
		"\n"
		"/* A recorded pause, divided by the speed factor in argument 0. */\n"
		"static guint\n"
		"scaled(GowlMacroContext *ctx, guint ms)\n"
		"{\n"
		"\tconst gchar *arg = gowl_macro_get_arg(ctx, 0);\n"
		"\tgdouble speed = arg != NULL ? g_ascii_strtod(arg, NULL) : 1.0;\n"
		"\n"
		"\tif (speed < 0.05 || speed > 100.0)\n"
		"\t\tspeed = 1.0;\n"
		"\treturn (guint)((gdouble)ms / speed);\n"
		"}\n"
		"\n"
		"G_MODULE_EXPORT gboolean\n"
		"gowl_macro_run(GowlMacroContext *ctx)\n"
		"{\n"
		"%s"
		"\treturn TRUE;\n"
		"}\n",
		stamp, st.steps, st.duration_ms / 1000,
		(st.duration_ms % 1000) / 100, body->str);
	g_string_free(body, TRUE);

	if (stats != NULL)
		*stats = st;
	return g_string_free(src, FALSE);
}
