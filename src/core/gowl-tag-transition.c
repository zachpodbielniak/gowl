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
 * Which tag transition runs.
 *
 * Every transition is its own module -- the cube, slide, fade, the
 * sphere, the page curl -- and several are usually loaded at once (cmacs
 * loads them all).  Each one watches tag switches; only ONE may animate
 * a given switch, or two sheets would fight over the output.  This file
 * is the arbiter:
 *
 *   - a module REGISTERS its name and a label for the toast;
 *   - the config's `tag-transition' SELECTS one, or "random", or "none";
 *   - for each switch on each output, gowl_compositor_tag_transition_pick()
 *     answers which name animates it.  Every module asks; the one named
 *     runs and the rest stand down.
 *
 * The pick is cached per output against the tag set it was made for, so
 * "random" is decided ONCE per switch: whichever module asks first, the
 * others get the same answer, and the answer only changes when the
 * output's tags change again.  That is what lets sixteen independent
 * modules agree without knowing about each other.
 *
 * Modules are separate shared objects and cannot call one another, which
 * is why the registry lives in the compositor rather than in a "host"
 * module.
 */

#include "gowl-core-private.h"
#include "gowl-compositor.h"
#include "../ipc/gowl-ipc.h"

#include <string.h>

#define GOWL_TAG_TRANSITION_REGISTRY_KEY "gowl-tag-transition-registry"
#define GOWL_TAG_TRANSITION_PICK_KEY     "gowl-tag-transition-pick"

/*
 * The order Super+: steps through, written out rather than registration
 * order -- which is whatever order the modules happened to load in, and
 * would make the key a different tour on every machine.
 *
 * The cube first, as the default.  Then the flat ones, the solids, the
 * shaders, and last the three that break the desktop into pieces, so a
 * tour from the default meets the quiet ones before the loud ones.
 * Anything registered that is not listed here (a third-party module)
 * follows, in registration order; "random" is always the last stop.
 */
static const gchar *const canonical_order[] = {
	"cube", "slide", "fade", "zoom", "iris",
	"flip", "pagecurl", "carousel", "sphere",
	"burn", "ripple", "pixelate", "glitch", "melt",
	"shatter", "explode", "implode",
	NULL
};

typedef struct {
	gchar *name;
	gchar *label;
	guint  seq;        /* registration order, for names not listed above */
} GowlTagTransition;

typedef struct {
	GPtrArray *entries;   /* GowlTagTransition* */
	guint      next_seq;
} GowlTagTransitionRegistry;

/* What was decided for an output's current tags. */
typedef struct {
	guint32  tagset;
	gboolean valid;
	gchar   *pick;        /* NULL: no transition for this switch */
	gchar   *last_random; /* so "random" does not repeat itself */
} GowlTagTransitionPick;

static void
entry_free(gpointer data)
{
	GowlTagTransition *e = (GowlTagTransition *)data;

	g_free(e->name);
	g_free(e->label);
	g_free(e);
}

static void
registry_free(gpointer data)
{
	GowlTagTransitionRegistry *r = (GowlTagTransitionRegistry *)data;

	g_ptr_array_unref(r->entries);
	g_free(r);
}

static void
pick_free(gpointer data)
{
	GowlTagTransitionPick *p = (GowlTagTransitionPick *)data;

	g_free(p->pick);
	g_free(p->last_random);
	g_free(p);
}

static GowlTagTransitionRegistry *
registry_get(GowlCompositor *self)
{
	GowlTagTransitionRegistry *r = g_object_get_data(
		G_OBJECT(self), GOWL_TAG_TRANSITION_REGISTRY_KEY);

	if (r == NULL) {
		r = g_new0(GowlTagTransitionRegistry, 1);
		r->entries = g_ptr_array_new_with_free_func(entry_free);
		g_object_set_data_full(G_OBJECT(self),
		                       GOWL_TAG_TRANSITION_REGISTRY_KEY,
		                       r, registry_free);
	}
	return r;
}

static GowlTagTransition *
registry_find(GowlTagTransitionRegistry *r, const gchar *name)
{
	guint i;

	for (i = 0; name != NULL && i < r->entries->len; i++) {
		GowlTagTransition *e = g_ptr_array_index(r->entries, i);

		if (g_ascii_strcasecmp(e->name, name) == 0)
			return e;
	}
	return NULL;
}

static gint
canonical_index(const gchar *name)
{
	gint i;

	for (i = 0; canonical_order[i] != NULL; i++)
		if (g_strcmp0(canonical_order[i], name) == 0)
			return i;
	return G_MAXINT;
}

static gint
entry_compare(gconstpointer a, gconstpointer b)
{
	const GowlTagTransition *x = *(const GowlTagTransition *const *)a;
	const GowlTagTransition *y = *(const GowlTagTransition *const *)b;
	gint ix = canonical_index(x->name);
	gint iy = canonical_index(y->name);

	if (ix != iy)
		return ix < iy ? -1 : 1;
	return x->seq < y->seq ? -1 : (x->seq > y->seq ? 1 : 0);
}

/* Every output's cached pick is stale once the menu of choices or the
 * selection changes. */
static void
picks_forget(GowlCompositor *self)
{
	GList *l;

	for (l = self->monitors; l != NULL; l = l->next) {
		GowlTagTransitionPick *p = g_object_get_data(
			G_OBJECT(l->data), GOWL_TAG_TRANSITION_PICK_KEY);

		if (p != NULL)
			p->valid = FALSE;
	}
}

/**
 * gowl_compositor_register_tag_transition:
 * @self: the compositor
 * @name: the transition's name, lower case, as `tag-transition' names it
 * @label: (nullable): what the toast calls it ("Page curl"); @name when
 *   %NULL
 *
 * Offers a transition.  Idempotent: registering a name twice keeps one
 * entry (and updates its label), so a module may register from every
 * hook that might be its first.
 */
void
gowl_compositor_register_tag_transition(
	GowlCompositor *self,
	const gchar    *name,
	const gchar    *label
){
	GowlTagTransitionRegistry *r;
	GowlTagTransition *e;

	g_return_if_fail(GOWL_IS_COMPOSITOR(self));
	g_return_if_fail(name != NULL && *name != '\0');

	r = registry_get(self);
	e = registry_find(r, name);
	if (e != NULL) {
		if (label != NULL && g_strcmp0(e->label, label) != 0) {
			g_free(e->label);
			e->label = g_strdup(label);
		}
		return;
	}
	e = g_new0(GowlTagTransition, 1);
	e->name = g_ascii_strdown(name, -1);
	e->label = g_strdup(label != NULL ? label : name);
	e->seq = r->next_seq++;
	g_ptr_array_add(r->entries, e);
	g_ptr_array_sort(r->entries, entry_compare);
	picks_forget(self);
}

/**
 * gowl_compositor_unregister_tag_transition:
 * @self: the compositor
 * @name: a registered name
 *
 * Withdraws a transition, when its module is deactivated.
 */
void
gowl_compositor_unregister_tag_transition(
	GowlCompositor *self,
	const gchar    *name
){
	GowlTagTransitionRegistry *r;
	GowlTagTransition *e;

	g_return_if_fail(GOWL_IS_COMPOSITOR(self));

	r = registry_get(self);
	e = registry_find(r, name);
	if (e == NULL)
		return;
	g_ptr_array_remove(r->entries, e);
	picks_forget(self);
}

/**
 * gowl_compositor_list_tag_transitions:
 * @self: the compositor
 *
 * The registered transitions in the order Super+: steps through them.
 * "random" is not in it: it is a selection, not a transition.
 *
 * Returns: (transfer full) (array zero-terminated=1): the names
 */
gchar **
gowl_compositor_list_tag_transitions(GowlCompositor *self)
{
	GowlTagTransitionRegistry *r;
	GPtrArray *out;
	guint i;

	g_return_val_if_fail(GOWL_IS_COMPOSITOR(self), NULL);

	r = registry_get(self);
	out = g_ptr_array_new();
	for (i = 0; i < r->entries->len; i++)
		g_ptr_array_add(out, g_strdup(
			((GowlTagTransition *)g_ptr_array_index(r->entries, i))->name));
	g_ptr_array_add(out, NULL);
	return (gchar **)g_ptr_array_free(out, FALSE);
}

/**
 * gowl_compositor_tag_transition_label:
 * @self: the compositor
 * @name: a transition name, "random" or "none"
 *
 * Returns: (transfer none): what the toast calls @name
 */
const gchar *
gowl_compositor_tag_transition_label(GowlCompositor *self, const gchar *name)
{
	GowlTagTransition *e;

	g_return_val_if_fail(GOWL_IS_COMPOSITOR(self), "");

	if (g_strcmp0(name, "random") == 0)
		return "Random";
	if (name == NULL || g_strcmp0(name, "none") == 0)
		return "None";
	e = registry_find(registry_get(self), name);
	return e != NULL ? e->label : name;
}

/**
 * gowl_compositor_get_tag_transition:
 * @self: the compositor
 *
 * Returns: (transfer none): the selection: a transition's name,
 *   "random" or "none"
 */
const gchar *
gowl_compositor_get_tag_transition(GowlCompositor *self)
{
	g_return_val_if_fail(GOWL_IS_COMPOSITOR(self), "none");

	return self->config != NULL
		? gowl_config_get_tag_transition(self->config) : "none";
}

/*
 * The selection changed: the toast, the event and the log.  The toast is
 * the same `toast-requested' the layout and the backdrop use, so it looks
 * like them, and it names the TRANSITION rather than just "Slide",
 * because a bare word on the screen after Super+: says nothing about
 * what changed.
 */
static void
announce(GowlCompositor *self, const gchar *name)
{
	if (self->selmon != NULL) {
		g_autofree gchar *text = g_strdup_printf("Transition: %s",
			gowl_compositor_tag_transition_label(self, name));

		g_signal_emit_by_name(self, "toast-requested", self->selmon, text);
	}
	if (self->ipc != NULL)
		gowl_ipc_push_event(self->ipc, "EVENT transition %s", name);
	g_info("tag transition: %s", name);
}

/*
 * The tour: every registered transition in order, then "random", then
 * round again.  From a selection that is not on the tour ("none", or a
 * module since unloaded) the next stop is the first.
 */
static gchar *
tour_step(GowlCompositor *self, gint direction)
{
	g_auto(GStrv) names = gowl_compositor_list_tag_transitions(self);
	const gchar *cur = gowl_compositor_get_tag_transition(self);
	guint n = g_strv_length(names);
	guint stops = n + 1;   /* + "random" */
	gint at = -1;
	guint i;

	if (n == 0)
		return NULL;
	for (i = 0; i < n; i++)
		if (g_strcmp0(names[i], cur) == 0)
			at = (gint)i;
	if (at < 0 && g_strcmp0(cur, "random") == 0)
		at = (gint)n;

	if (at < 0)
		at = direction >= 0 ? 0 : (gint)n;
	else
		at = (gint)(((guint)at + stops + (direction >= 0 ? 1u : stops - 1u))
		            % stops);
	return g_strdup((guint)at == n ? "random" : names[at]);
}

/**
 * gowl_compositor_set_tag_transition:
 * @self: the compositor
 * @name: a registered transition, "random", "none", or "next" / "prev"
 *   to step through them
 *
 * Selects the transition every tag switch uses from now on, says so with
 * a toast, and pushes `EVENT transition NAME'.  A name nothing has
 * registered is refused, so a typo cannot leave the switch silently
 * instant.
 *
 * Returns: %TRUE when @name was accepted
 */
gboolean
gowl_compositor_set_tag_transition(GowlCompositor *self, const gchar *name)
{
	g_autofree gchar *want = NULL;

	g_return_val_if_fail(GOWL_IS_COMPOSITOR(self), FALSE);

	if (self->config == NULL || name == NULL || *name == '\0')
		return FALSE;

	if (g_ascii_strcasecmp(name, "next") == 0)
		want = tour_step(self, 1);
	else if (g_ascii_strcasecmp(name, "prev") == 0)
		want = tour_step(self, -1);
	else
		want = g_ascii_strdown(name, -1);
	if (want == NULL)
		return FALSE;

	if (g_strcmp0(want, "random") != 0 && g_strcmp0(want, "none") != 0
	    && registry_find(registry_get(self), want) == NULL)
		return FALSE;

	/* Already in force: accepted, and quiet.  An embedder pushing its
	 * setting at every session start (cmacs does) must not toast
	 * "Transition: Cube" at every login. */
	if (g_strcmp0(want, gowl_compositor_get_tag_transition(self)) == 0)
		return TRUE;

	gowl_config_set_tag_transition(self->config, want);
	picks_forget(self);
	announce(self, want);
	return TRUE;
}

/**
 * gowl_compositor_cycle_tag_transition:
 * @self: the compositor
 * @direction: 1 for the next stop, -1 for the previous
 *
 * What Super+: does: one step round the tour of loaded transitions,
 * with "random" as its last stop.
 */
void
gowl_compositor_cycle_tag_transition(GowlCompositor *self, gint direction)
{
	g_return_if_fail(GOWL_IS_COMPOSITOR(self));

	gowl_compositor_set_tag_transition(self, direction >= 0 ? "next"
	                                                        : "prev");
}

/**
 * gowl_compositor_tag_transition_pick:
 * @self: the compositor
 * @monitor: an output
 *
 * Which transition animates the switch @monitor has just made (or is
 * showing now): the selection, or for "random" one of the registered
 * transitions chosen for this switch.  Decided once per output per tag
 * set, so every module asking about the same switch hears the same name;
 * the next change of tags is a new decision.
 *
 * Returns: (transfer none) (nullable): the name, or %NULL when nothing
 *   should animate -- "none", or a selection no loaded module provides
 */
const gchar *
gowl_compositor_tag_transition_pick(GowlCompositor *self, GowlMonitor *monitor)
{
	GowlTagTransitionRegistry *r;
	GowlTagTransitionPick *p;
	const gchar *sel;
	guint32 cur;

	g_return_val_if_fail(GOWL_IS_COMPOSITOR(self), NULL);
	if (monitor == NULL)
		return NULL;

	cur = monitor->tagset[monitor->seltags];
	p = g_object_get_data(G_OBJECT(monitor), GOWL_TAG_TRANSITION_PICK_KEY);
	if (p == NULL) {
		p = g_new0(GowlTagTransitionPick, 1);
		g_object_set_data_full(G_OBJECT(monitor),
		                       GOWL_TAG_TRANSITION_PICK_KEY, p, pick_free);
	}
	if (p->valid && p->tagset == cur)
		return p->pick;

	r = registry_get(self);
	sel = gowl_compositor_get_tag_transition(self);
	g_clear_pointer(&p->pick, g_free);

	if (g_strcmp0(sel, "random") == 0 && r->entries->len > 0) {
		guint n = r->entries->len;
		guint i = (guint)g_random_int_range(0, (gint32)n);
		GowlTagTransition *e = g_ptr_array_index(r->entries, i);

		/* Not the same one twice running when there is a choice:
		 * "random" that shows the same thing twice reads as stuck. */
		if (n > 1 && g_strcmp0(e->name, p->last_random) == 0)
			e = g_ptr_array_index(r->entries, (i + 1 + (guint)
				g_random_int_range(0, (gint32)(n - 1))) % n);
		p->pick = g_strdup(e->name);
		g_free(p->last_random);
		p->last_random = g_strdup(e->name);
	} else if (registry_find(r, sel) != NULL) {
		p->pick = g_strdup(sel);
	}
	p->tagset = cur;
	p->valid = TRUE;
	return p->pick;
}

/**
 * gowl_compositor_tag_transition_is:
 * @self: the compositor
 * @monitor: an output
 * @name: a transition's name
 *
 * Whether @name is the one to animate @monitor's current switch -- what
 * a transition module asks before it starts.  Which OUTPUTS animate at
 * all (`tag-transition-all-monitors', `cube-all-monitors') is the
 * module's own check.
 *
 * Returns: %TRUE when @name should run
 */
gboolean
gowl_compositor_tag_transition_is(
	GowlCompositor *self,
	GowlMonitor    *monitor,
	const gchar    *name
){
	g_return_val_if_fail(GOWL_IS_COMPOSITOR(self), FALSE);

	return g_strcmp0(gowl_compositor_tag_transition_pick(self, monitor),
	                 name) == 0;
}
