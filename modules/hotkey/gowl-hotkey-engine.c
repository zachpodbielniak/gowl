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
 * gowl-hotkey-engine.c - AutoHotkey's keyboard hook, as a state machine.
 *
 * A press is decided once, in this order, and the decision is kept with
 * the held input so its release follows it whatever changes meanwhile:
 *
 *   1. a CUSTOM COMBINATION (`CapsLock & j') whose prefix is held;
 *   2. the input as a PREFIX of some combination: swallowed (unless a
 *      combination says `~'), and its own hotkeys wait for the release,
 *      which fires them only if nothing else was pressed meanwhile;
 *   3. a LONG PRESS (`hold:'): swallowed until it is decided -- held past
 *      its time it fires; released early (or interrupted by another key)
 *      it was a tap: the plain hotkeys on the key fire, or with none the
 *      key is put back, so Escape still types Escape;
 *   4. a LONE MODIFIER (`LShift', `LWin'): never swallowed, so it still
 *      modifies; its hotkeys fire on release when no other input came
 *      in between (`~' fires on the press instead) -- a tap;
 *   5. a DOUBLE TAP (`double:'): the first press is held back for the
 *      double time; a second press in time fires it, and otherwise the
 *      plain hotkeys on the key fire, or the key is put back.  `~' lets
 *      both presses through and fires on the second;
 *   6. plain hotkeys -- one, the best -- and `UP' hotkeys, armed for the
 *      release; swallowed unless `~';
 *   7. a REMAP, only when nothing above claimed the input: swallowed, the
 *      target pressed now and released with it;
 *   8. a swallowing MODE eats what is left.
 *
 * "Best" is the highest priority, then a hotkey with a context over one
 * without (AutoHotkey's #HotIf variants beat the global one), then more
 * modifiers, then the one loaded last.  A hotkey only counts when its
 * mode is in force, it is enabled, the suspend switch spares it, its
 * modifiers match (exactly, or at least with `*'; a combination is its
 * own wildcard; `<'/`>' ask for the held key's hand) and -- asked last,
 * of matches only -- the caller allows it.
 *
 * Injected input: the caller's own (own_level >= 0) only fires hotkeys
 * whose level is BELOW its send level, so a hotkey's Send never fires
 * hotkeys of the default level 0; `$' hotkeys never fire on injected
 * input at all; and someone else's (a KVM, a macro) is ignored unless
 * the caller accepts it.  A key put back by the engine is the caller's
 * own at level 0.
 */

#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "gowl-hotkey"

#include "gowl-hotkey-engine.h"
#include "gowl-enums.h"

#include <string.h>
#include <xkbcommon/xkbcommon.h>

/* The modifiers a hotkey can ask for: not the locks, not Mod2 */
#define HK_MOD_MASK (GOWL_KEY_MOD_SHIFT | GOWL_KEY_MOD_CTRL \
                     | GOWL_KEY_MOD_ALT | GOWL_KEY_MOD_LOGO \
                     | GOWL_KEY_MOD_MOD3 | GOWL_KEY_MOD_MOD5)

/* One held input and how its press was decided */
typedef struct {
	GowlHotkeyInputKind kind;
	guint32    code;           /* keycode or button */
	guint32    keysym;         /* keys: the base keysym */
	gboolean   consumed;       /* the press was swallowed: so is the release */
	gboolean   own;
	gboolean   touched;        /* something else was pressed since */
	HkHotkey  *up;             /* an UP hotkey to fire on release */
	HkHotkey  *remap;          /* a remap whose target is down */
	gboolean   is_prefix;
	GPtrArray *solo;           /* prefix: its own hotkeys, for the release */
	GPtrArray *lone;           /* lone modifier: hotkeys for an untouched release */
	HkHotkey  *hold;           /* a long press, while undecided */
	gint64     hold_deadline;
	GPtrArray *taps;           /* hold: the plain hotkeys, for a tap */
	gboolean   replayed;       /* its press was put back; the release passes */
} Held;

/* A double tap's first press, waiting for the second */
typedef struct {
	HkHotkey  *hotkey;
	GowlHotkeyInputKind kind;
	guint32    code;
	gint64     deadline;
	GPtrArray *singles;        /* plain hotkeys on the key, for no second tap */
	gboolean   released;       /* the first press is already up */
} Pending;

struct _HkEngine {
	GPtrArray   *hotkeys;      /* HkHotkey*, a ref each */
	HkAllowFunc  allow;
	gpointer     allow_data;
	gboolean     accept_synthetic;
	gboolean     suspended;
	gboolean     swallow;
	GHashTable  *held_keys;    /* keycode -> Held* */
	GHashTable  *held_buttons; /* button -> Held* */
	GPtrArray   *pending;      /* Pending* */
	GHashTable  *last_tap;     /* hotkey id -> gint64* (~ double taps) */
	gchar       *prior;
	gint64       prior_ms;
	GPtrArray   *history;      /* HkHistoryEntry*, oldest first */
};

/* ── Lifecycle ─────────────────────────────────────────────────────── */

static GPtrArray *
hotkey_list(void)
{
	return g_ptr_array_new_with_free_func((GDestroyNotify)hk_hotkey_unref);
}

static void
held_free(gpointer p)
{
	Held *h = p;

	if (h == NULL)
		return;
	hk_hotkey_unref(h->up);
	hk_hotkey_unref(h->remap);
	hk_hotkey_unref(h->hold);
	if (h->solo != NULL)
		g_ptr_array_unref(h->solo);
	if (h->lone != NULL)
		g_ptr_array_unref(h->lone);
	if (h->taps != NULL)
		g_ptr_array_unref(h->taps);
	g_free(h);
}

static void
pending_free(gpointer p)
{
	Pending *pd = p;

	hk_hotkey_unref(pd->hotkey);
	if (pd->singles != NULL)
		g_ptr_array_unref(pd->singles);
	g_free(pd);
}

static void
history_free(gpointer p)
{
	HkHistoryEntry *h = p;

	g_free(h->fired);
	g_free(h);
}

HkEngine *
hk_engine_new(void)
{
	HkEngine *e = g_new0(HkEngine, 1);

	e->hotkeys = hotkey_list();
	e->held_keys = g_hash_table_new_full(g_direct_hash, g_direct_equal,
	                                     NULL, held_free);
	e->held_buttons = g_hash_table_new_full(g_direct_hash, g_direct_equal,
	                                        NULL, held_free);
	e->pending = g_ptr_array_new_with_free_func(pending_free);
	e->last_tap = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
	                                    g_free);
	e->history = g_ptr_array_new_with_free_func(history_free);
	return e;
}

void
hk_engine_free(HkEngine *e)
{
	if (e == NULL)
		return;
	g_ptr_array_unref(e->hotkeys);
	g_hash_table_unref(e->held_keys);
	g_hash_table_unref(e->held_buttons);
	g_ptr_array_unref(e->pending);
	g_hash_table_unref(e->last_tap);
	g_free(e->prior);
	g_ptr_array_unref(e->history);
	g_free(e);
}

/**
 * hk_engine_set_hotkeys: (skip)
 * @e: the engine
 * @hotkeys: (element-type HkHotkey): the hotkeys in force; each is
 *   referenced, so the caller's set may go
 *
 * What is held keeps the hotkeys it was decided with: an UP hotkey still
 * fires and a remap target is still released after a reload.
 */
void
hk_engine_set_hotkeys(
	HkEngine  *e,
	GPtrArray *hotkeys
){
	guint i;

	g_return_if_fail(e != NULL);
	g_ptr_array_set_size(e->hotkeys, 0);
	for (i = 0; hotkeys != NULL && i < hotkeys->len; i++)
		g_ptr_array_add(e->hotkeys,
		                hk_hotkey_ref(g_ptr_array_index(hotkeys, i)));
}

void
hk_engine_set_allow_func(
	HkEngine    *e,
	HkAllowFunc  func,
	gpointer     user_data
){
	g_return_if_fail(e != NULL);
	e->allow = func;
	e->allow_data = user_data;
}

void
hk_engine_set_accept_synthetic(
	HkEngine *e,
	gboolean  accept
){
	g_return_if_fail(e != NULL);
	e->accept_synthetic = accept;
}

void
hk_engine_set_suspended(
	HkEngine *e,
	gboolean  suspended
){
	g_return_if_fail(e != NULL);
	e->suspended = suspended;
}

gboolean
hk_engine_get_suspended(HkEngine *e)
{
	g_return_val_if_fail(e != NULL, FALSE);
	return e->suspended;
}

/* The mode in force swallows what no hotkey in it takes. */
void
hk_engine_set_swallow(
	HkEngine *e,
	gboolean  swallow
){
	g_return_if_fail(e != NULL);
	e->swallow = swallow;
}

GArray *
hk_out_new(void)
{
	return g_array_new(FALSE, TRUE, sizeof(HkOut));
}

/* Drops the hotkey references in @out and empties it. */
void
hk_out_clear(GArray *out)
{
	guint i;

	if (out == NULL)
		return;
	for (i = 0; i < out->len; i++)
		hk_hotkey_unref(g_array_index(out, HkOut, i).hotkey);
	g_array_set_size(out, 0);
}

/* ── Output ────────────────────────────────────────────────────────── */

static void
out_fire(
	HkEngine *e,
	GArray   *out,
	HkHotkey *hk,
	gint64    now
){
	HkOut o;

	memset(&o, 0, sizeof o);
	o.kind = HK_OUT_FIRE;
	o.hotkey = hk_hotkey_ref(hk);
	o.why = "hotkey";
	g_array_append_val(out, o);
	g_free(e->prior);
	e->prior = g_strdup(hk->id);
	e->prior_ms = now;
	if (e->history->len > 0) {
		HkHistoryEntry *last = g_ptr_array_index(e->history,
		                                         e->history->len - 1);

		if (last->fired == NULL)
			last->fired = g_strdup(hk->id);
	}
}

/* Puts input back: the key or button as it was, pressed or released. */
static void
out_replay(
	GArray              *out,
	GowlHotkeyInputKind  kind,
	guint32              code,
	gboolean             pressed
){
	HkOut o;

	memset(&o, 0, sizeof o);
	o.why = "replay";
	o.pressed = pressed;
	if (kind == GOWL_HOTKEY_INPUT_BUTTON) {
		o.kind = HK_OUT_BUTTON;
		o.button = code;
	} else {
		o.kind = HK_OUT_KEY;
		o.keycode = code;
	}
	g_array_append_val(out, o);
}

static void
out_remap(
	GArray   *out,
	HkHotkey *hk,
	gboolean  pressed
){
	HkOut o;

	memset(&o, 0, sizeof o);
	o.why = "remap";
	o.pressed = pressed;
	o.mods = hk->remap_mods;
	if (hk->remap_to.kind == GOWL_HOTKEY_INPUT_BUTTON) {
		o.kind = HK_OUT_BUTTON;
		o.button = hk->remap_to.button;
	} else {
		o.kind = HK_OUT_KEY;
		o.keycode = hk->remap_to.keycode;
		o.keysym = hk->remap_to.keysym;
	}
	g_array_append_val(out, o);
}

/* ── Matching ──────────────────────────────────────────────────────── */

static GHashTable *
held_table(
	HkEngine            *e,
	GowlHotkeyInputKind  kind
){
	return kind == GOWL_HOTKEY_INPUT_BUTTON ? e->held_buttons : e->held_keys;
}

/* Does held input @h stand for @in? */
static gboolean
held_is(
	const Held            *h,
	const GowlHotkeyInput *in
){
	if (in->kind == GOWL_HOTKEY_INPUT_BUTTON)
		return h->kind == GOWL_HOTKEY_INPUT_BUTTON && h->code == in->button;
	if (in->kind != GOWL_HOTKEY_INPUT_KEY || h->kind != GOWL_HOTKEY_INPUT_KEY)
		return FALSE;
	return gowl_hotkey_input_matches_key(in, h->code, h->keysym, h->keysym);
}

/* Is @in held (other than by the event's own entry, @self)? */
static gboolean
input_held(
	HkEngine              *e,
	const GowlHotkeyInput *in,
	const Held            *self
){
	GHashTableIter it;
	gpointer v;

	g_hash_table_iter_init(&it, held_table(e, in->kind));
	while (g_hash_table_iter_next(&it, NULL, &v))
		if (v != self && held_is(v, in))
			return TRUE;
	return FALSE;
}

/* Is every modifier in @bits held on the @side (-1 left, 1 right)? */
static gboolean
held_on_side(
	HkEngine *e,
	guint     bits,
	gint      side
){
	guint bit;

	for (bit = 1; bit != 0 && bit <= bits; bit <<= 1) {
		GHashTableIter it;
		gpointer v;
		gboolean found = FALSE;

		if (!(bits & bit))
			continue;
		g_hash_table_iter_init(&it, e->held_keys);
		while (g_hash_table_iter_next(&it, NULL, &v)) {
			Held *h = v;

			if (gowl_hotkey_keysym_modifier(h->keysym) == bit
			    && gowl_hotkey_keysym_side(h->keysym) == side) {
				found = TRUE;
				break;
			}
		}
		if (!found)
			return FALSE;
	}
	return TRUE;
}

static gboolean
event_matches_input(
	const HkEvent         *ev,
	const GowlHotkeyInput *in
){
	switch (ev->kind) {
	case GOWL_HOTKEY_INPUT_KEY:
		return gowl_hotkey_input_matches_key(in, ev->keycode, ev->keysym,
		                                     ev->base_keysym);
	case GOWL_HOTKEY_INPUT_BUTTON:
	case GOWL_HOTKEY_INPUT_WHEEL:
		return in->kind == ev->kind && in->button == ev->button;
	default:
		return FALSE;
	}
}

static gboolean
mode_ok(
	const HkHotkey *hk,
	const gchar    *mode
){
	gboolean in_default = mode == NULL || g_strcmp0(mode, "default") == 0;

	if (hk->mode == NULL)
		return in_default;
	if (g_strcmp0(hk->mode, "*") == 0)
		return TRUE;
	return !in_default && g_strcmp0(hk->mode, mode) == 0;
}

/* Everything but the caller's say: is @hk a candidate for @ev? */
static gboolean
candidate(
	HkEngine      *e,
	HkHotkey      *hk,
	const HkEvent *ev,
	const gchar   *mode,
	const Held    *self
){
	guint mods;

	if (!hk->enabled || !mode_ok(hk, mode))
		return FALSE;
	if (e->suspended && !hk->suspend_exempt)
		return FALSE;
	if (ev->own_level >= 0) {
		if ((hk->spec.flags & GOWL_HOTKEY_FLAG_NO_SYNTHETIC)
		    || ev->own_level <= hk->level || hk->is_remap)
			return FALSE;
	} else if (ev->synthetic
	           && (hk->spec.flags & GOWL_HOTKEY_FLAG_NO_SYNTHETIC)) {
		return FALSE;
	}
	if (!event_matches_input(ev, &hk->spec.trigger))
		return FALSE;
	if (hk->spec.prefix.kind != GOWL_HOTKEY_INPUT_NONE)
		return input_held(e, &hk->spec.prefix, self);

	/* A modifier key is not its own modifier: LShift's press already
	 * has Shift down */
	mods = ev->mods & HK_MOD_MASK;
	if (ev->kind == GOWL_HOTKEY_INPUT_KEY)
		mods &= ~gowl_hotkey_keysym_modifier(ev->base_keysym);
	if (hk->spec.flags & GOWL_HOTKEY_FLAG_WILDCARD) {
		if ((mods & hk->spec.mods) != hk->spec.mods)
			return FALSE;
	} else if (mods != hk->spec.mods) {
		return FALSE;
	}
	if (hk->spec.left != 0 && !held_on_side(e, hk->spec.left, -1))
		return FALSE;
	if (hk->spec.right != 0 && !held_on_side(e, hk->spec.right, 1))
		return FALSE;
	return TRUE;
}

static gint
n_bits(guint v)
{
	gint n = 0;

	for (; v != 0; v &= v - 1)
		n++;
	return n;
}

/* Best first: priority, a context, more modifiers, loaded later */
static gint
compare_hotkeys(
	gconstpointer pa,
	gconstpointer pb
){
	const HkHotkey *a = *(const HkHotkey * const *)pa;
	const HkHotkey *b = *(const HkHotkey * const *)pb;
	gint ca;
	gint cb;

	if (a->priority != b->priority)
		return b->priority - a->priority;
	ca = (a->context != NULL || !hk_condition_is_empty(&a->cond)) ? 1 : 0;
	cb = (b->context != NULL || !hk_condition_is_empty(&b->cond)) ? 1 : 0;
	if (ca != cb)
		return cb - ca;
	if (n_bits(a->spec.mods) != n_bits(b->spec.mods))
		return n_bits(b->spec.mods) - n_bits(a->spec.mods);
	return (gint)b->order - (gint)a->order;
}

static gboolean
allowed(
	HkEngine *e,
	HkHotkey *hk
){
	return e->allow == NULL || e->allow(hk, e->allow_data);
}

/* The candidates for @ev, best first (not yet asked of the caller). */
static GPtrArray *
collect(
	HkEngine      *e,
	const HkEvent *ev,
	const gchar   *mode,
	const Held    *self
){
	GPtrArray *c = g_ptr_array_new();
	guint i;

	for (i = 0; i < e->hotkeys->len; i++) {
		HkHotkey *hk = g_ptr_array_index(e->hotkeys, i);

		if (candidate(e, hk, ev, mode, self))
			g_ptr_array_add(c, hk);
	}
	g_ptr_array_sort(c, compare_hotkeys);
	return c;
}

typedef enum {
	PICK_COMBO,
	PICK_HOLD,
	PICK_DOUBLE,
	PICK_PLAIN,       /* fires on the press */
	PICK_UP,          /* fires on the release */
	PICK_REMAP
} Pick;

static gboolean
is_pick(
	const HkHotkey *hk,
	Pick            pick
){
	gboolean combo = hk->spec.prefix.kind != GOWL_HOTKEY_INPUT_NONE;
	gboolean up = (hk->spec.flags & GOWL_HOTKEY_FLAG_ON_RELEASE) != 0;

	switch (pick) {
	case PICK_COMBO:
		return combo && !hk->is_remap;
	case PICK_REMAP:
		return hk->is_remap;
	default:
		break;
	}
	if (combo || hk->is_remap)
		return FALSE;
	switch (pick) {
	case PICK_HOLD:
		return hk->hold_ms > 0;
	case PICK_DOUBLE:
		return hk->double_ms > 0;
	case PICK_PLAIN:
		return hk->hold_ms == 0 && hk->double_ms == 0 && !up;
	case PICK_UP:
		return hk->hold_ms == 0 && hk->double_ms == 0 && up;
	default:
		return FALSE;
	}
}

/* The best candidate of a kind the caller allows, or NULL. */
static HkHotkey *
best(
	HkEngine  *e,
	GPtrArray *cands,
	Pick       pick
){
	guint i;

	for (i = 0; i < cands->len; i++) {
		HkHotkey *hk = g_ptr_array_index(cands, i);

		if (is_pick(hk, pick) && allowed(e, hk))
			return hk;
	}
	return NULL;
}

/* Every allowed candidate of a kind, as a referenced list (or NULL). */
static GPtrArray *
all_of(
	HkEngine  *e,
	GPtrArray *cands,
	Pick       pick
){
	GPtrArray *out = NULL;
	guint i;

	for (i = 0; i < cands->len; i++) {
		HkHotkey *hk = g_ptr_array_index(cands, i);

		if (!is_pick(hk, pick) || !allowed(e, hk))
			continue;
		if (out == NULL)
			out = hotkey_list();
		g_ptr_array_add(out, hk_hotkey_ref(hk));
	}
	return out;
}

/* Is @ev the prefix of a combination in force?  @passthrough says
 * whether one of them asks (with `~') for the prefix to stay usable. */
static gboolean
is_prefix_input(
	HkEngine      *e,
	const HkEvent *ev,
	const gchar   *mode,
	gboolean      *passthrough
){
	gboolean any = FALSE;
	guint i;

	*passthrough = FALSE;
	for (i = 0; i < e->hotkeys->len; i++) {
		HkHotkey *hk = g_ptr_array_index(e->hotkeys, i);

		if (hk->spec.prefix.kind == GOWL_HOTKEY_INPUT_NONE || hk->is_remap
		    || !hk->enabled || !mode_ok(hk, mode)
		    || (e->suspended && !hk->suspend_exempt)
		    || !event_matches_input(ev, &hk->spec.prefix))
			continue;
		if (!allowed(e, hk))
			continue;
		any = TRUE;
		if (hk->spec.flags & GOWL_HOTKEY_FLAG_PASSTHROUGH)
			*passthrough = TRUE;
	}
	return any;
}

/* ── History ───────────────────────────────────────────────────────── */

static HkHistoryEntry *
history_push(
	HkEngine      *e,
	const HkEvent *ev,
	gint64         now
){
	HkHistoryEntry *h = g_new0(HkHistoryEntry, 1);

	h->time_ms = now;
	h->kind = ev->kind;
	h->code = ev->kind == GOWL_HOTKEY_INPUT_KEY ? ev->keycode : ev->button;
	h->keysym = ev->base_keysym;
	h->pressed = ev->pressed;
	h->synthetic = ev->synthetic;
	h->own = ev->own_level >= 0;
	if (e->history->len >= HK_HISTORY_SIZE)
		g_ptr_array_remove_index(e->history, 0);
	g_ptr_array_add(e->history, h);
	return h;
}

/* ── Deciding a tap ────────────────────────────────────────────────── */

/* A tap of a key whose hotkeys waited for it: fire them (a double-tap
 * hotkey among them counts the tap), or report that nothing did. */
static gboolean
fire_taps(
	HkEngine  *e,
	GPtrArray *list,
	gint64     now,
	GArray    *out
){
	gboolean fired = FALSE;
	guint i;

	for (i = 0; list != NULL && i < list->len; i++) {
		HkHotkey *hk = g_ptr_array_index(list, i);

		if (hk->double_ms > 0) {
			gint64 *last = g_hash_table_lookup(e->last_tap, hk->id);

			if (last != NULL && now - *last <= (gint64)hk->double_ms) {
				g_hash_table_remove(e->last_tap, hk->id);
				out_fire(e, out, hk, now);
				fired = TRUE;
			} else {
				gint64 *t = g_new(gint64, 1);

				*t = now;
				g_hash_table_replace(e->last_tap, g_strdup(hk->id), t);
			}
			continue;
		}
		out_fire(e, out, hk, now);
		fired = TRUE;
		/* One plain hotkey per tap, like a press */
		break;
	}
	return fired;
}

/* A long press decided as a tap: its plain hotkeys, or the key back. */
static void
hold_as_tap(
	HkEngine *e,
	Held     *h,
	gboolean  released,
	gint64    now,
	GArray   *out
){
	hk_hotkey_unref(g_steal_pointer(&h->hold));
	if (h->taps != NULL && h->taps->len > 0) {
		fire_taps(e, h->taps, now, out);
		return;
	}
	if (!h->consumed)
		return;
	out_replay(out, h->kind, h->code, TRUE);
	if (released)
		out_replay(out, h->kind, h->code, FALSE);
	else
		h->replayed = TRUE;
}

/* A double tap's first press decided as a single: its plain hotkeys,
 * or the key back. */
static void
double_as_single(
	HkEngine *e,
	Pending  *pd,
	gint64    now,
	GArray   *out
){
	Held *h;

	if (pd->singles != NULL && pd->singles->len > 0) {
		fire_taps(e, pd->singles, now, out);
		return;
	}
	out_replay(out, pd->kind, pd->code, TRUE);
	if (pd->released) {
		out_replay(out, pd->kind, pd->code, FALSE);
		return;
	}
	/* Still down: its release now passes through */
	h = g_hash_table_lookup(held_table(e, pd->kind),
	                        GUINT_TO_POINTER(pd->code));
	if (h != NULL)
		h->replayed = TRUE;
}

/* Another input arrived: every long press and double tap still waiting
 * is a tap.  Returns TRUE when something was put back -- the new input
 * must then follow it rather than overtake it. */
static gboolean
interrupt_pending(
	HkEngine      *e,
	const HkEvent *ev,
	gint64         now,
	GArray        *out
){
	GHashTableIter it;
	gpointer v;
	guint before = out->len;
	guint i;

	g_hash_table_iter_init(&it, e->held_keys);
	while (g_hash_table_iter_next(&it, NULL, &v)) {
		Held *h = v;

		if (h->hold != NULL && !(ev->kind == GOWL_HOTKEY_INPUT_KEY
		                         && h->code == ev->keycode))
			hold_as_tap(e, h, FALSE, now, out);
	}
	g_hash_table_iter_init(&it, e->held_buttons);
	while (g_hash_table_iter_next(&it, NULL, &v)) {
		Held *h = v;

		if (h->hold != NULL && !(ev->kind == GOWL_HOTKEY_INPUT_BUTTON
		                         && h->code == ev->button))
			hold_as_tap(e, h, FALSE, now, out);
	}
	for (i = 0; i < e->pending->len; ) {
		Pending *pd = g_ptr_array_index(e->pending, i);
		guint32 code = ev->kind == GOWL_HOTKEY_INPUT_KEY ? ev->keycode
		                                                 : ev->button;

		if (pd->kind == ev->kind && pd->code == code) {
			i++;
			continue;
		}
		double_as_single(e, pd, now, out);
		g_ptr_array_remove_index(e->pending, i);
	}
	for (i = before; i < out->len; i++)
		if (g_array_index(out, HkOut, i).kind != HK_OUT_FIRE)
			return TRUE;
	return FALSE;
}

/* ── Presses ───────────────────────────────────────────────────────── */

static gboolean
press(
	HkEngine      *e,
	const HkEvent *ev,
	const gchar   *mode,
	gint64         now,
	GArray        *out
){
	g_autoptr(GPtrArray) cands = NULL;
	GHashTable *table = held_table(e, ev->kind);
	guint32 code = ev->kind == GOWL_HOTKEY_INPUT_KEY ? ev->keycode
	                                                 : ev->button;
	GHashTableIter it;
	gpointer v;
	Held *h;
	HkHotkey *hk;
	gboolean consume = FALSE;
	gboolean matched = FALSE;
	gboolean is_mod;
	gboolean put_back;
	gboolean passthrough;
	guint i;

	/* Every input held before this one has now been "touched" */
	g_hash_table_iter_init(&it, e->held_keys);
	while (g_hash_table_iter_next(&it, NULL, &v))
		((Held *)v)->touched = TRUE;
	g_hash_table_iter_init(&it, e->held_buttons);
	while (g_hash_table_iter_next(&it, NULL, &v))
		((Held *)v)->touched = TRUE;
	put_back = ev->own_level < 0 && interrupt_pending(e, ev, now, out);

	h = g_new0(Held, 1);
	h->kind = ev->kind;
	h->code = code;
	h->keysym = xkb_keysym_to_lower(ev->base_keysym);
	h->own = ev->own_level >= 0;
	is_mod = ev->kind == GOWL_HOTKEY_INPUT_KEY
	         && gowl_hotkey_keysym_modifier(ev->base_keysym) != 0;

	cands = collect(e, ev, mode, h);

	/* 0. the second press of a double tap waiting for it */
	for (i = 0; ev->own_level < 0 && i < e->pending->len; i++) {
		Pending *pd = g_ptr_array_index(e->pending, i);

		if (pd->kind != ev->kind || pd->code != code)
			continue;
		out_fire(e, out, pd->hotkey, now);
		g_ptr_array_remove_index(e->pending, i);
		consume = TRUE;
		matched = TRUE;
		goto done;
	}

	/* 1. a combination whose prefix is held */
	if ((hk = best(e, cands, PICK_COMBO)) != NULL) {
		if (hk->spec.flags & GOWL_HOTKEY_FLAG_ON_RELEASE)
			h->up = hk_hotkey_ref(hk);
		else
			out_fire(e, out, hk, now);
		consume = !(hk->spec.flags & GOWL_HOTKEY_FLAG_PASSTHROUGH);
		matched = TRUE;
		goto done;
	}

	/* 2. a prefix */
	if (ev->own_level < 0 && ev->kind != GOWL_HOTKEY_INPUT_WHEEL
	    && is_prefix_input(e, ev, mode, &passthrough)) {
		h->is_prefix = TRUE;
		h->solo = all_of(e, cands, PICK_PLAIN);
		if (h->solo == NULL)
			h->solo = all_of(e, cands, PICK_UP);
		consume = !passthrough && !is_mod;
		matched = TRUE;
		goto done;
	}

	/* 3. a long press */
	if (ev->own_level < 0 && (hk = best(e, cands, PICK_HOLD)) != NULL) {
		h->hold = hk_hotkey_ref(hk);
		h->hold_deadline = now + hk->hold_ms;
		h->taps = all_of(e, cands, PICK_PLAIN);
		/* A modifier is never swallowed: it would stay down in the
		 * keyboard's state anyway, swallowed or not */
		consume = !(hk->spec.flags & GOWL_HOTKEY_FLAG_PASSTHROUGH) && !is_mod;
		matched = TRUE;
		goto done;
	}

	/* 4. a lone modifier: fires on an untouched release */
	if (is_mod) {
		for (i = 0; i < cands->len; i++) {
			hk = g_ptr_array_index(cands, i);
			if (hk->spec.prefix.kind != GOWL_HOTKEY_INPUT_NONE
			    || hk->is_remap || hk->hold_ms > 0 || !allowed(e, hk))
				continue;
			matched = TRUE;
			if ((hk->spec.flags & GOWL_HOTKEY_FLAG_PASSTHROUGH)
			    && hk->double_ms == 0
			    && !(hk->spec.flags & GOWL_HOTKEY_FLAG_ON_RELEASE)) {
				out_fire(e, out, hk, now);
				continue;
			}
			if (h->lone == NULL)
				h->lone = hotkey_list();
			g_ptr_array_add(h->lone, hk_hotkey_ref(hk));
		}
		if (matched)
			goto done;
	}

	/* 5. a double tap */
	if (ev->own_level < 0 && (hk = best(e, cands, PICK_DOUBLE)) != NULL) {
		matched = TRUE;
		if (hk->spec.flags & GOWL_HOTKEY_FLAG_PASSTHROUGH) {
			/* Both presses go through; the second within time fires */
			GPtrArray *one = hotkey_list();

			g_ptr_array_add(one, hk_hotkey_ref(hk));
			fire_taps(e, one, now, out);
			g_ptr_array_unref(one);
			if ((hk = best(e, cands, PICK_PLAIN)) != NULL) {
				out_fire(e, out, hk, now);
				consume = !(hk->spec.flags & GOWL_HOTKEY_FLAG_PASSTHROUGH);
			}
			goto done;
		} else {
			Pending *pd = g_new0(Pending, 1);

			pd->hotkey = hk_hotkey_ref(hk);
			pd->kind = ev->kind;
			pd->code = code;
			pd->deadline = now + hk->double_ms;
			pd->singles = all_of(e, cands, PICK_PLAIN);
			g_ptr_array_add(e->pending, pd);
			consume = TRUE;
			goto done;
		}
	}

	/* 6. plain and UP hotkeys */
	if ((hk = best(e, cands, PICK_PLAIN)) != NULL) {
		out_fire(e, out, hk, now);
		consume = !(hk->spec.flags & GOWL_HOTKEY_FLAG_PASSTHROUGH);
		matched = TRUE;
	}
	if (ev->kind != GOWL_HOTKEY_INPUT_WHEEL
	    && (hk = best(e, cands, PICK_UP)) != NULL) {
		h->up = hk_hotkey_ref(hk);
		consume = consume || !(hk->spec.flags & GOWL_HOTKEY_FLAG_PASSTHROUGH);
		matched = TRUE;
	}
	if (matched)
		goto done;

	/* 7. a remap */
	if (ev->kind != GOWL_HOTKEY_INPUT_WHEEL && ev->own_level < 0
	    && (hk = best(e, cands, PICK_REMAP)) != NULL) {
		h->remap = hk_hotkey_ref(hk);
		out_remap(out, hk, TRUE);
		consume = TRUE;
		matched = TRUE;
		goto done;
	}

	/* 8. a swallowing mode eats the rest -- but not modifiers, which a
	 * mode's own hotkeys may need, and not our own injections */
	if (e->swallow && !is_mod && ev->own_level < 0)
		consume = TRUE;

done:
	/* Something was put back ahead of this input: it must follow, so it
	 * is swallowed and put back after (and its release then passes) */
	if (put_back && !consume && ev->kind != GOWL_HOTKEY_INPUT_WHEEL) {
		out_replay(out, ev->kind, code, TRUE);
		consume = TRUE;
		h->replayed = TRUE;
	}
	h->consumed = consume && !h->replayed;
	/* Our own injection of a key that is physically held -- a key put
	 * back -- must not replace the physical entry, which still owns the
	 * release */
	if (ev->kind == GOWL_HOTKEY_INPUT_WHEEL
	    || (h->own && g_hash_table_contains(table, GUINT_TO_POINTER(code))))
		held_free(h);
	else
		g_hash_table_replace(table, GUINT_TO_POINTER(code), h);
	(void)matched;
	return consume;
}

/* ── Releases ──────────────────────────────────────────────────────── */

static gboolean
release(
	HkEngine      *e,
	const HkEvent *ev,
	gint64         now,
	GArray        *out
){
	GHashTable *table = held_table(e, ev->kind);
	guint32 code = ev->kind == GOWL_HOTKEY_INPUT_KEY ? ev->keycode
	                                                 : ev->button;
	Held *h = NULL;
	gboolean consume;
	guint i;

	/* Our own release of a key that is physically held (a put-back
	 * tap) leaves the physical entry for the physical release */
	h = g_hash_table_lookup(table, GUINT_TO_POINTER(code));
	if (h == NULL || (ev->own_level >= 0 && !h->own))
		return FALSE;
	h = NULL;
	if (!g_hash_table_steal_extended(table, GUINT_TO_POINTER(code), NULL,
	                                 (gpointer *)&h))
		return FALSE;
	consume = h->consumed;

	/* A double tap's first press, released while its second is awaited */
	for (i = 0; i < e->pending->len; i++) {
		Pending *pd = g_ptr_array_index(e->pending, i);

		if (pd->kind == h->kind && pd->code == code) {
			pd->released = TRUE;
			consume = TRUE;
		}
	}

	if (h->hold != NULL) {
		/* Released before its time: a tap */
		hold_as_tap(e, h, TRUE, now, out);
		consume = TRUE;
	}
	if (h->replayed)
		consume = FALSE;
	if (h->is_prefix && !h->touched && h->solo != NULL)
		fire_taps(e, h->solo, now, out);
	if (h->lone != NULL && !h->touched)
		fire_taps(e, h->lone, now, out);
	if (h->up != NULL)
		out_fire(e, out, h->up, now);
	if (h->remap != NULL)
		out_remap(out, h->remap, FALSE);
	held_free(h);
	return consume;
}

/**
 * hk_engine_input: (skip)
 * @e: the engine
 * @ev: one input event
 * @mode: (nullable): the key mode in force ("default" or %NULL for none)
 * @now_ms: the time, monotonic milliseconds
 * @out: (element-type HkOut): what follows from it is appended here
 *
 * Returns: %TRUE when the event must not reach the window
 */
gboolean
hk_engine_input(
	HkEngine      *e,
	const HkEvent *ev,
	const gchar   *mode,
	gint64         now_ms,
	GArray        *out
){
	HkHistoryEntry *rec;
	gboolean consume;

	g_return_val_if_fail(e != NULL && ev != NULL && out != NULL, FALSE);

	/* Someone else's injected input is not ours to decide, unless the
	 * caller says it is (a KVM keyboard is a keyboard) */
	if (ev->synthetic && ev->own_level < 0 && !e->accept_synthetic)
		return FALSE;

	rec = history_push(e, ev, now_ms);
	if (ev->kind == GOWL_HOTKEY_INPUT_WHEEL || ev->pressed)
		consume = press(e, ev, mode, now_ms, out);
	else
		consume = release(e, ev, now_ms, out);
	rec->consumed = consume;
	return consume;
}

/* ── Time ──────────────────────────────────────────────────────────── */

/**
 * hk_engine_next_deadline: (skip)
 * @e: the engine
 *
 * Returns: when hk_engine_tick() next has something to decide, or -1
 */
gint64
hk_engine_next_deadline(HkEngine *e)
{
	GHashTableIter it;
	gpointer v;
	gint64 next = -1;
	guint i;

	g_return_val_if_fail(e != NULL, -1);
	g_hash_table_iter_init(&it, e->held_keys);
	while (g_hash_table_iter_next(&it, NULL, &v)) {
		Held *h = v;

		if (h->hold != NULL && (next < 0 || h->hold_deadline < next))
			next = h->hold_deadline;
	}
	g_hash_table_iter_init(&it, e->held_buttons);
	while (g_hash_table_iter_next(&it, NULL, &v)) {
		Held *h = v;

		if (h->hold != NULL && (next < 0 || h->hold_deadline < next))
			next = h->hold_deadline;
	}
	for (i = 0; i < e->pending->len; i++) {
		Pending *pd = g_ptr_array_index(e->pending, i);

		if (next < 0 || pd->deadline < next)
			next = pd->deadline;
	}
	return next;
}

static void
tick_table(
	HkEngine   *e,
	GHashTable *table,
	gint64      now,
	GArray     *out
){
	GHashTableIter it;
	gpointer v;

	g_hash_table_iter_init(&it, table);
	while (g_hash_table_iter_next(&it, NULL, &v)) {
		Held *h = v;

		if (h->hold == NULL || h->hold_deadline > now)
			continue;
		/* Held long enough: the long press fires, and its release is
		 * swallowed like its press was */
		out_fire(e, out, h->hold, now);
		hk_hotkey_unref(g_steal_pointer(&h->hold));
	}
}

/**
 * hk_engine_tick: (skip)
 * @e: the engine
 * @now_ms: the time
 * @out: (element-type HkOut): what follows
 *
 * Decides the long presses and double taps whose time has come.
 */
void
hk_engine_tick(
	HkEngine *e,
	gint64    now_ms,
	GArray   *out
){
	guint i;

	g_return_if_fail(e != NULL && out != NULL);
	tick_table(e, e->held_keys, now_ms, out);
	tick_table(e, e->held_buttons, now_ms, out);
	for (i = 0; i < e->pending->len; ) {
		Pending *pd = g_ptr_array_index(e->pending, i);

		if (pd->deadline > now_ms) {
			i++;
			continue;
		}
		double_as_single(e, pd, now_ms, out);
		g_ptr_array_remove_index(e->pending, i);
	}
}

/**
 * hk_engine_reset: (skip)
 * @e: the engine
 * @out: (element-type HkOut): remap targets to release
 *
 * Forgets everything in flight -- on a lock, a deactivation -- letting
 * go of any remap target still held.  Nothing pending fires.
 */
void
hk_engine_reset(
	HkEngine *e,
	GArray   *out
){
	GHashTableIter it;
	gpointer v;

	g_return_if_fail(e != NULL && out != NULL);
	g_hash_table_iter_init(&it, e->held_keys);
	while (g_hash_table_iter_next(&it, NULL, &v))
		if (((Held *)v)->remap != NULL)
			out_remap(out, ((Held *)v)->remap, FALSE);
	g_hash_table_iter_init(&it, e->held_buttons);
	while (g_hash_table_iter_next(&it, NULL, &v))
		if (((Held *)v)->remap != NULL)
			out_remap(out, ((Held *)v)->remap, FALSE);
	g_hash_table_remove_all(e->held_keys);
	g_hash_table_remove_all(e->held_buttons);
	g_ptr_array_set_size(e->pending, 0);
	g_hash_table_remove_all(e->last_tap);
}

/* ── Queries ───────────────────────────────────────────────────────── */

/**
 * hk_engine_key_is_down: (skip)
 * @e: the engine
 * @input: a key or button
 *
 * Returns: %TRUE while it is physically held (AutoHotkey's
 *   GetKeyState(key, "P"))
 */
gboolean
hk_engine_key_is_down(
	HkEngine              *e,
	const GowlHotkeyInput *input
){
	g_return_val_if_fail(e != NULL && input != NULL, FALSE);
	return input_held(e, input, NULL);
}

guint
hk_engine_n_held(HkEngine *e)
{
	g_return_val_if_fail(e != NULL, 0);
	return g_hash_table_size(e->held_keys)
	       + g_hash_table_size(e->held_buttons);
}

/**
 * hk_engine_get_prior: (skip)
 * @e: the engine
 * @time_ms: (out) (optional): when it fired
 *
 * Returns: (nullable): the id of the hotkey that fired last
 *   (AutoHotkey's A_PriorHotkey)
 */
const gchar *
hk_engine_get_prior(
	HkEngine *e,
	gint64   *time_ms
){
	g_return_val_if_fail(e != NULL, NULL);
	if (time_ms != NULL)
		*time_ms = e->prior_ms;
	return e->prior;
}

/**
 * hk_engine_get_history: (skip)
 * @e: the engine
 *
 * Returns: (transfer none) (element-type HkHistoryEntry): the last
 *   %HK_HISTORY_SIZE input events, oldest first
 */
GPtrArray *
hk_engine_get_history(HkEngine *e)
{
	g_return_val_if_fail(e != NULL, NULL);
	return e->history;
}
