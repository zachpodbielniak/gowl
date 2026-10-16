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
 * The `keywheel' widget: a radial cheat sheet of every keybinding.
 *
 * A keyboard glyph in the bar whose panel is a centred overlay: a big
 * canvas drawing the compositor's own keybinds as a wheel -- one spoke
 * per key, one ring per modifier set -- with a search field and the
 * binds the wheel cannot show (no Super, Emacs) as rows beneath it.
 * Enter, or a second click on a cell, runs the bind through the
 * compositor's own dispatch, exactly as pressing the chord would.
 *
 * Where the data comes from.  The compositor's config, read on the
 * compositor thread when the panel is built after being closed, and
 * copied into a BarKeywheelModel (bar-keywheel-model.c) so nothing
 * here keeps a pointer into a config a reload may replace.  Under
 * cmacs --gowl the same config carries every `gowl-add-keybind', so
 * cmacs's binds appear with no extra plumbing.  cmacs can also push a
 * fifth face, a list of notable Emacs bindings, as `emacs.count' and
 * `emacs.<n>' settings.
 *
 * Threads.  There is no worker: everything this widget does -- read
 * the config, lay out the model, paint, handle keys -- is bounded,
 * in-memory work, and all of it happens on the compositor thread.  The
 * mutex is there anyway, because the configure hook can be reached
 * from a settings push and the discipline costs nothing.  It is NEVER
 * held across a call back into the host or the compositor: closing
 * the panel calls panel_closed, and a dispatched bind can reconfigure
 * the bar, and both take it.
 */

#include <math.h>
#include <string.h>
#include <pango/pangocairo.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#include "core/gowl-compositor.h"
#include "config/gowl-config.h"
#include "gowl-enums.h"
#include "barkit/gowl-bar-host.h"

#include "bar-internal.h"
#include "bar-keywheel-model.h"

/* U+2328 KEYBOARD. */
#define KW_GLYPH "\xe2\x8c\xa8"

/* The canvas height for a face that is not a wheel: header and dots. */
#define KW_LIST_CANVAS_H (56)

/* A config is not unbounded in practice, but a snapshot runs on the
   compositor thread, so it is bounded on principle. */
#define KW_MAX_BINDS (2000)

typedef struct {
	GMutex             lock;
	BarKeywheelModel  *model;        /* the snapshot; replaced whole */
	GPtrArray         *emacs;        /* BarKeywheelEmacs*, from settings */
	BarKeywheelCursor  cursor;
	gchar             *query;        /* the search field's text */
	gboolean           stale;        /* re-read the config at next build */
	gboolean           focus_search; /* the next open focuses search */
	gboolean           reopening;    /* closing only to reopen */
	gint               painted_w;    /* the canvas as last painted, */
	gint               painted_h;    /*   for click hit-testing */
} Keywheel;

/* ----------------------------------------------------------------
 * Model ownership
 * ---------------------------------------------------------------- */

/* Out-of-range selections are worse than none: clamp after a swap. */
static void
kw_clamp_cursor_locked(Keywheel *self)
{
	BarKeywheelCursor *c = &self->cursor;

	if ((guint)c->face >= bar_keywheel_face_count(self->model))
		bar_keywheel_cursor_reset(c);
	if (c->spoke >= (gint)bar_keywheel_n_spokes(self->model, c->face))
		c->spoke = -1;
	if (c->row >= (gint)bar_keywheel_list_len(self->model, c->face))
		c->row = -1;
}

/**
 * bar_keywheel_widget_install:
 * @data: the widget's instance data
 * @model: (transfer full): a finished model
 *
 * Makes @model the wheel's snapshot, replacing the previous one.  This
 * is the one way a model reaches the widget: the snapshot of the
 * compositor's config goes through it, and so does a test that builds
 * a model from a fixture -- the panel cannot tell the two apart.
 */
void
bar_keywheel_widget_install(gpointer data, BarKeywheelModel *model)
{
	Keywheel *self = data;
	BarKeywheelModel *old;

	g_return_if_fail(self != NULL && model != NULL);

	g_mutex_lock(&self->lock);
	bar_keywheel_model_set_emacs(model, self->emacs);
	old = self->model;
	self->model = model;
	self->stale = FALSE;
	kw_clamp_cursor_locked(self);
	g_mutex_unlock(&self->lock);

	bar_keywheel_model_free(old);
}

/*
 * Copies the compositor's keybinds into a new model.
 *
 * Only binds of the default key mode: a bind inside a mode (`resize')
 * only exists while that mode is entered, so drawing it on the wheel
 * would claim a chord does something it does not, and Enter could not
 * run it.  Compositor thread only.
 *
 * Returns NULL when there is no compositor to read -- a test, or a
 * bar that has not started -- and the caller keeps what it had.
 */
static BarKeywheelModel *
kw_snapshot(void)
{
	const BarEnv *env;
	GowlConfig *config;
	GArray *keybinds;
	GEnumClass *actions;
	BarKeywheelModel *model;
	guint i;

	env = bar_env();
	if (env == NULL || env->compositor == NULL)
		return NULL;
	config = gowl_compositor_get_config(GOWL_COMPOSITOR(env->compositor));
	if (config == NULL)
		return NULL;
	keybinds = gowl_config_get_keybinds(config);

	model = bar_keywheel_model_new();
	actions = (GEnumClass *)g_type_class_ref(GOWL_TYPE_ACTION);
	for (i = 0; keybinds != NULL && i < keybinds->len &&
	            i < KW_MAX_BINDS; i++) {
		GowlKeybindEntry *kb = &g_array_index(keybinds,
		                                      GowlKeybindEntry, i);
		GEnumValue *ev;
		g_autofree gchar *action = NULL;

		if (kb->mode != NULL || kb->keysym == 0)
			continue;
		/* The config's spelling: `kill_client', not the GEnum
		   nick `kill-client'. */
		ev = g_enum_get_value(actions, kb->action);
		action = g_strdup((ev != NULL) ? ev->value_nick : "none");
		g_strdelimit(action, "-", '_');
		bar_keywheel_model_add_bind(model, kb->modifiers, kb->keysym,
		                            kb->flags, action, kb->arg,
		                            kb->desc);
	}
	g_type_class_unref(actions);
	bar_keywheel_model_finish(model);
	return model;
}

/* Bring the snapshot up to date if the panel was closed since. */
static void
kw_refresh_model(Keywheel *self)
{
	BarKeywheelModel *fresh;
	gboolean stale, have;

	g_mutex_lock(&self->lock);
	stale = self->stale;
	have  = self->model != NULL;
	g_mutex_unlock(&self->lock);
	if (!stale && have)
		return;

	fresh = kw_snapshot();
	if (fresh == NULL && have) {
		g_mutex_lock(&self->lock);
		self->stale = FALSE;
		g_mutex_unlock(&self->lock);
		return;
	}
	if (fresh == NULL) {
		fresh = bar_keywheel_model_new();
		bar_keywheel_model_finish(fresh);
	}
	bar_keywheel_widget_install(self, fresh);
}

/* ----------------------------------------------------------------
 * Lifecycle and settings
 * ---------------------------------------------------------------- */

static gpointer
kw_create(GowlBarPlugin *plugin)
{
	Keywheel *self;

	(void)plugin;
	self = g_new0(Keywheel, 1);
	g_mutex_init(&self->lock);
	self->emacs = g_ptr_array_new_with_free_func(
		(GDestroyNotify)bar_keywheel_emacs_free);
	bar_keywheel_cursor_reset(&self->cursor);
	self->stale = TRUE;
	return self;
}

static void
kw_destroy(GowlBarPlugin *plugin, gpointer data)
{
	Keywheel *self = data;

	(void)plugin;
	if (self == NULL)
		return;
	bar_keywheel_model_free(self->model);
	g_ptr_array_unref(self->emacs);
	g_free(self->query);
	g_mutex_clear(&self->lock);
	g_free(self);
}

/*
 * Settings changed.  The only ones read here are the Emacs face's:
 * cmacs pushes `emacs.count' and `emacs.<n>', and the list is rebuilt
 * whole (at most BAR_KEYWHEEL_MAX_EMACS entries, each field bounded and
 * made valid UTF-8 by the parser).  Colours and the size are read where
 * they are used, so a change to them needs nothing here.
 */
static void
kw_configure(GowlBarPlugin *plugin, gpointer data, GHashTable *settings)
{
	Keywheel *self = data;
	GPtrArray *emacs;
	gint count, i;

	(void)settings;
	if (self == NULL)
		return;

	count = gowl_bar_plugin_get_setting_int(plugin, "emacs.count", 0);
	count = CLAMP(count, 0, BAR_KEYWHEEL_MAX_EMACS);
	emacs = g_ptr_array_new_with_free_func(
		(GDestroyNotify)bar_keywheel_emacs_free);
	for (i = 0; i < count; i++) {
		gchar key[32];
		BarKeywheelEmacs *e;

		g_snprintf(key, sizeof(key), "emacs.%d", i);
		e = bar_keywheel_emacs_parse(
			gowl_bar_plugin_get_setting(plugin, key));
		if (e != NULL)
			g_ptr_array_add(emacs, e);
	}

	g_mutex_lock(&self->lock);
	g_ptr_array_unref(self->emacs);
	self->emacs = emacs;
	if (self->model != NULL)
		bar_keywheel_model_set_emacs(self->model, emacs);
	kw_clamp_cursor_locked(self);
	g_mutex_unlock(&self->lock);
	gowl_bar_plugin_request_panel_refresh(plugin);
}

static gint
kw_interval(GowlBarPlugin *plugin, gpointer data)
{
	(void)plugin;
	(void)data;
	/* Only the tooltip's count moves, and only on a config change. */
	return 30;
}

/* The bar label: a glyph, its colour, and how many binds there are. */
static void
kw_poll(GowlBarPlugin *plugin, gpointer data)
{
	const BarEnv *env;
	const gchar *icon;
	guint n = 0;
	gboolean known = FALSE;

	(void)data;
	icon = gowl_bar_plugin_get_setting(plugin, "icon");
	gowl_bar_plugin_set_icon(plugin, (icon != NULL && icon[0] != '\0')
	                                 ? icon : KW_GLYPH);
	gowl_bar_plugin_set_label(plugin, NULL);
	bar_plugin_apply_color(plugin,
	                       gowl_bar_plugin_get_setting(plugin, "color"),
	                       GOWL_BAR_COLOR_TEXT);

	/* Cheap: one pointer walk to an array length.  The poll runs on
	   the compositor thread, which is the only place this is safe. */
	env = bar_env();
	if (env != NULL && env->compositor != NULL) {
		GowlConfig *config = gowl_compositor_get_config(
			GOWL_COMPOSITOR(env->compositor));
		GArray *kb = (config != NULL)
			? gowl_config_get_keybinds(config) : NULL;

		if (kb != NULL) {
			n = kb->len;
			known = TRUE;
		}
	}
	if (known) {
		g_autofree gchar *tip = g_strdup_printf(
			"Key wheel: %u keybindings", n);

		gowl_bar_plugin_set_tooltip(plugin, tip);
	} else {
		gowl_bar_plugin_set_tooltip(plugin, "Key wheel");
	}
}

/* ----------------------------------------------------------------
 * Running a bind
 * ---------------------------------------------------------------- */

/*
 * Runs one chord through the compositor's own dispatch, after closing
 * the panel -- a bind that moves focus or opens a window should act on
 * the desktop, not on an overlay that is about to vanish.
 *
 * gowl_compositor_dispatch_keybind() is the path a real key press
 * takes, so a `spawn' bind launches, a `custom' bind reaches cmacs, and
 * a duplicate chord runs the same (first) entry the key would.  A bind
 * marked `release' only fires on the release side, so it goes through
 * gowl_compositor_dispatch_key() with pressed FALSE.  No lock held:
 * both calls can come back into this widget.
 */
static void
kw_run_chord(GowlBarPlugin *plugin, guint modifiers, guint keysym,
             guint flags)
{
	GowlBarHost *host;
	const BarEnv *env;
	gboolean ran;
	g_autofree gchar *combo = bar_keywheel_combo(modifiers, keysym);

	host = gowl_bar_plugin_get_host(plugin);
	if (host != NULL)
		gowl_bar_host_close_panel(host);

	env = bar_env();
	if (env == NULL || env->compositor == NULL) {
		gowl_bar_plugin_notify(plugin, GOWL_BAR_TOAST_NORMAL,
			"Key wheel: not running it",
			"There is no compositor to run the bind in.");
		return;
	}

	if (flags & BAR_KEYWHEEL_FLAG_RELEASE)
		ran = gowl_compositor_dispatch_key(
			GOWL_COMPOSITOR(env->compositor), modifiers, keysym,
			FALSE, FALSE);
	else
		ran = gowl_compositor_dispatch_keybind(
			GOWL_COMPOSITOR(env->compositor), modifiers, keysym);
	if (!ran) {
		g_autofree gchar *body = g_strdup_printf(
			"%s did not run. A key mode may be active, or the "
			"config changed since the wheel opened.", combo);

		gowl_bar_plugin_notify(plugin, GOWL_BAR_TOAST_NORMAL,
			"Key wheel: nothing ran", body);
	}
}

/* Run the bind under the cursor, if there is one. */
static void
kw_run_cursor(GowlBarPlugin *plugin, Keywheel *self)
{
	guint mods = 0, keysym = 0, flags = 0;
	gboolean ok;

	g_mutex_lock(&self->lock);
	ok = bar_keywheel_run_target(self->model, &self->cursor, &mods,
	                             &keysym, &flags);
	g_mutex_unlock(&self->lock);
	if (ok)
		kw_run_chord(plugin, mods, keysym, flags);
}

/* Run bind @index of the snapshot (a search result or a list row). */
static void
kw_run_index(GowlBarPlugin *plugin, Keywheel *self, guint index)
{
	const BarKeywheelBind *b;
	guint mods = 0, keysym = 0, flags = 0;
	gboolean ok = FALSE;

	g_mutex_lock(&self->lock);
	b = bar_keywheel_bind(self->model, index);
	if (b != NULL) {
		mods   = b->modifiers;
		keysym = b->keysym;
		flags  = b->flags;
		ok     = TRUE;
	}
	g_mutex_unlock(&self->lock);
	if (ok)
		kw_run_chord(plugin, mods, keysym, flags);
}

/*
 * Run an Emacs command from the Emacs face.
 *
 * Not by eval-ing the command: an `emacsctl eval' runs inside cmacs's
 * RPC dispatch, which binds `inhibit-interaction', so a command that
 * reads a key or a file name would fail (or wedge).  The cmacs half,
 * `cmacs-gowl-bar-keywheel-run', checks the name against the list it
 * pushed and schedules the command on a timer, outside the RPC, where
 * it may prompt like any command.  Both the program and the form are
 * shell-quoted; the name is Elisp-quoted inside the form.
 */
static void
kw_run_emacs(GowlBarPlugin *plugin, Keywheel *self, guint index)
{
	g_autofree gchar *command = NULL;
	g_autofree gchar *quoted = NULL;
	g_autofree gchar *form = NULL;
	g_autofree gchar *q_prog = NULL;
	g_autofree gchar *q_form = NULL;
	g_autofree gchar *cmdline = NULL;
	GowlBarHost *host;
	const gchar *prog;

	g_mutex_lock(&self->lock);
	if (self->model != NULL && self->model->emacs != NULL &&
	    index < self->model->emacs->len) {
		const BarKeywheelEmacs *e =
			g_ptr_array_index(self->model->emacs, index);

		command = g_strdup(e->command);
	}
	g_mutex_unlock(&self->lock);
	if (command == NULL)
		return;

	prog = gowl_bar_plugin_get_setting(plugin, "emacsctl");
	if (prog == NULL || prog[0] == '\0')
		prog = "emacsctl";
	quoted  = bar_elisp_quote(command);
	form    = g_strdup_printf("(cmacs-gowl-bar-keywheel-run \"%s\")",
	                          quoted);
	q_prog  = g_shell_quote(prog);
	q_form  = g_shell_quote(form);
	cmdline = g_strdup_printf("%s eval %s", q_prog, q_form);

	host = gowl_bar_plugin_get_host(plugin);
	if (host != NULL)
		gowl_bar_host_close_panel(host);
	gowl_bar_plugin_spawn(plugin, cmdline);
}

/* ----------------------------------------------------------------
 * The panel
 * ---------------------------------------------------------------- */

/* Pick a row's title: the description, else what it does. */
static gchar *
kw_bind_title(const BarKeywheelBind *b)
{
	if (b->desc != NULL)
		return g_strdup(b->desc);
	if (b->arg != NULL)
		return g_strdup_printf("%s %s", b->action, b->arg);
	return g_strdup(b->action);
}

static void
kw_add_bind_row(GowlBarPanel *panel, const BarKeywheelBind *b, guint index,
                gboolean selected)
{
	GowlBarPanelItem *row;
	g_autofree gchar *id = g_strdup_printf("run:%u", index);
	g_autofree gchar *title = kw_bind_title(b);
	g_autofree gchar *sub = NULL;

	if (b->desc != NULL)
		sub = (b->arg != NULL)
			? g_strdup_printf("%s %s", b->action, b->arg)
			: g_strdup(b->action);
	row = gowl_bar_panel_add_row(panel, id, NULL, title, sub);
	gowl_bar_panel_item_set_value(row, b->combo);
	gowl_bar_panel_item_set_selected(row, selected);
}

static void
kw_add_emacs_row(GowlBarPanel *panel, const BarKeywheelEmacs *e,
                 guint index, gboolean selected)
{
	GowlBarPanelItem *row;
	g_autofree gchar *id = g_strdup_printf("emacs:%u", index);

	row = gowl_bar_panel_add_row(panel, id, NULL,
		(e->doc != NULL) ? e->doc : e->command, e->command);
	gowl_bar_panel_item_set_value(row, e->key);
	gowl_bar_panel_item_set_selected(row, selected);
}

static GowlBarPanel *
kw_panel(GowlBarPlugin *plugin, gpointer data)
{
	Keywheel *self = data;
	GowlBarPanel *panel;
	GowlBarPanelItem *item;
	GowlBarHost *host;
	const GowlBarTheme *theme = NULL;
	BarKeywheelModel *m;
	BarKeywheelCursor c;
	gint w, h, pad = 12;
	guint i, n_binds;
	gboolean wheel;

	kw_refresh_model(self);

	bar_keywheel_parse_size(gowl_bar_plugin_get_setting(plugin, "size"),
	                        &w, &h);
	host = gowl_bar_plugin_get_host(plugin);
	if (host != NULL)
		theme = gowl_bar_host_get_theme(host);
	if (theme != NULL)
		pad = gowl_bar_theme_metric(theme, GOWL_BAR_METRIC_PAD_X);

	panel = gowl_bar_panel_new();
	gowl_bar_panel_set_width(panel, w + 2 * pad);
	gowl_bar_panel_set_centered(panel, TRUE);

	g_mutex_lock(&self->lock);
	m = self->model;
	c = self->cursor;
	n_binds = (m != NULL) ? m->binds->len : 0;
	wheel = bar_keywheel_face_is_wheel(c.face) &&
	        bar_keywheel_n_spokes(m, c.face) > 0;

	/* The canvas first, so it is the first stop for the keyboard.
	   AUTOFOCUS is an entry property today; set here it costs nothing
	   and starts working the day the host honours it for a canvas. */
	item = gowl_bar_panel_add_canvas(panel, "wheel",
	                                 wheel ? h : KW_LIST_CANVAS_H);
	gowl_bar_panel_item_set_prop(item, GOWL_BAR_ENTRY_PROP_AUTOFOCUS,
	                             "true");

	if (n_binds == 0)
		gowl_bar_panel_add_label(panel,
			"No keybinds available. The compositor has none "
			"configured, or the bar is not running inside gowl.");

	item = gowl_bar_panel_add_entry(panel, "search",
		"Search keybindings  (press / to jump here)", self->query);
	if (self->focus_search)
		gowl_bar_panel_item_set_prop(item,
			GOWL_BAR_ENTRY_PROP_AUTOFOCUS, "true");

	if (self->query != NULL && self->query[0] != '\0') {
		g_autoptr(GArray) hits_b = g_array_new(FALSE, FALSE,
		                                       sizeof(guint));
		g_autoptr(GArray) hits_e = g_array_new(FALSE, FALSE,
		                                       sizeof(guint));
		guint total;

		total = bar_keywheel_search(m, self->query,
		                            BAR_KEYWHEEL_MAX_RESULTS,
		                            hits_b, hits_e);
		if (total == 0)
			gowl_bar_panel_add_label(panel, "No keybinding matches.");
		for (i = 0; i < hits_b->len; i++) {
			guint idx = g_array_index(hits_b, guint, i);

			kw_add_bind_row(panel, bar_keywheel_bind(m, idx), idx,
			                FALSE);
		}
		for (i = 0; i < hits_e->len; i++) {
			guint idx = g_array_index(hits_e, guint, i);

			kw_add_emacs_row(panel,
				g_ptr_array_index(m->emacs, idx), idx, FALSE);
		}
		if (total > hits_b->len + hits_e->len) {
			g_autofree gchar *more = g_strdup_printf(
				"%u more; type more to narrow it down.",
				total - hits_b->len - hits_e->len);

			gowl_bar_panel_add_label(panel, more);
		}
	} else if (c.face == BAR_KEYWHEEL_FACE_BEYOND && m != NULL) {
		if (m->beyond->len == 0)
			gowl_bar_panel_add_label(panel,
				"Every keybinding uses Super.");
		for (i = 0; i < m->beyond->len; i++) {
			guint idx = g_array_index(m->beyond, guint, i);

			kw_add_bind_row(panel, bar_keywheel_bind(m, idx), idx,
			                c.row == (gint)i);
		}
	} else if (c.face == BAR_KEYWHEEL_FACE_EMACS && m != NULL &&
	           m->emacs != NULL) {
		for (i = 0; i < m->emacs->len; i++)
			kw_add_emacs_row(panel, g_ptr_array_index(m->emacs, i),
			                 i, c.row == (gint)i);
	}
	g_mutex_unlock(&self->lock);

	item = gowl_bar_panel_add_buttons(panel, "nav");
	gowl_bar_panel_add_button(item, "\xe2\x97\x80 Face", FALSE);
	gowl_bar_panel_add_button(item, "Run", FALSE);
	gowl_bar_panel_add_button(item, "Face \xe2\x96\xb6", FALSE);
	return panel;
}

static void
kw_panel_opened(GowlBarPlugin *plugin, gpointer data)
{
	Keywheel *self = data;

	(void)plugin;
	if (self == NULL)
		return;
	g_mutex_lock(&self->lock);
	/* The open that wanted the search field has had it. */
	self->focus_search = FALSE;
	g_mutex_unlock(&self->lock);
}

/*
 * Closed: next time, read the config again and start from Core.  Not
 * when the close is the first half of the reopen that puts the
 * keyboard in the search field -- that must look like nothing happened.
 */
static void
kw_panel_closed(GowlBarPlugin *plugin, gpointer data)
{
	Keywheel *self = data;

	(void)plugin;
	if (self == NULL)
		return;
	g_mutex_lock(&self->lock);
	if (!self->reopening) {
		bar_keywheel_cursor_reset(&self->cursor);
		g_clear_pointer(&self->query, g_free);
		self->stale = TRUE;
		self->focus_search = FALSE;
	}
	g_mutex_unlock(&self->lock);
}

/* The canvas size clicks are measured against. */
static void
kw_click_geom_locked(GowlBarPlugin *plugin, Keywheel *self,
                     BarKeywheelGeom *g)
{
	gint w = self->painted_w, h = self->painted_h;
	gboolean wheel;

	if (w <= 0 || h <= 0)
		bar_keywheel_parse_size(
			gowl_bar_plugin_get_setting(plugin, "size"), &w, &h);
	wheel = bar_keywheel_face_is_wheel(self->cursor.face) &&
	        bar_keywheel_n_spokes(self->model, self->cursor.face) > 0;
	bar_keywheel_geom((gdouble)w, (gdouble)h,
	                  bar_keywheel_n_spokes(self->model, self->cursor.face),
	                  bar_keywheel_face_count(self->model), wheel, g);
}

/*
 * A click on the canvas (canvas-local x in @index, y in @value), or
 * Return on the focused canvas (both -1).
 */
static void
kw_canvas_click(GowlBarPlugin *plugin, Keywheel *self, gint x, gdouble y)
{
	BarKeywheelGeom g;
	BarKeywheelHit hit;
	BarKeywheelRing ring;
	gint spoke, face;
	gboolean run = FALSE;

	if (x < 0 && y < 0.0) {
		kw_run_cursor(plugin, self);
		return;
	}

	g_mutex_lock(&self->lock);
	kw_click_geom_locked(plugin, self, &g);
	hit = bar_keywheel_hit(&g, (gdouble)x, y, &spoke, &ring, &face);
	switch (hit) {
	case BAR_KEYWHEEL_HIT_CELL:
		run = bar_keywheel_select_cell(self->model, &self->cursor,
		                               (guint)spoke, ring);
		break;
	case BAR_KEYWHEEL_HIT_DOT:
		bar_keywheel_cursor_face(self->model, &self->cursor,
		                         face - (gint)self->cursor.face);
		break;
	case BAR_KEYWHEEL_HIT_ALT:
		self->cursor.alt = !self->cursor.alt;
		break;
	case BAR_KEYWHEEL_HIT_NONE:
	default:
		break;
	}
	g_mutex_unlock(&self->lock);

	/* A second click on the selected cell runs it: panels have no
	   double-click. */
	if (run)
		kw_run_cursor(plugin, self);
}

static void
kw_action(GowlBarPlugin *plugin, gpointer data, const gchar *item_id,
          gint index, gdouble value, guint button)
{
	Keywheel *self = data;
	guint64 n;

	(void)button;
	if (self == NULL || item_id == NULL)
		return;

	if (strcmp(item_id, "wheel") == 0) {
		kw_canvas_click(plugin, self, index, value);
		return;
	}
	if (strcmp(item_id, "nav") == 0) {
		if (index == 1) {
			kw_run_cursor(plugin, self);
			return;
		}
		g_mutex_lock(&self->lock);
		bar_keywheel_cursor_face(self->model, &self->cursor,
		                         index == 0 ? -1 : 1);
		g_mutex_unlock(&self->lock);
		return;
	}
	if (g_str_has_prefix(item_id, "run:") &&
	    g_ascii_string_to_unsigned(item_id + 4, 10, 0, G_MAXUINT, &n,
	                               NULL)) {
		kw_run_index(plugin, self, (guint)n);
		return;
	}
	if (g_str_has_prefix(item_id, "emacs:") &&
	    g_ascii_string_to_unsigned(item_id + 6, 10, 0, G_MAXUINT, &n,
	                               NULL)) {
		kw_run_emacs(plugin, self, (guint)n);
		return;
	}
}

/*
 * Typing in the search field filters; Return in it runs the first
 * result -- the bind somebody searched for is usually the one they
 * want to run.
 */
static void
kw_panel_text(GowlBarPlugin *plugin, gpointer data, const gchar *item_id,
              const gchar *text, gboolean submitted)
{
	Keywheel *self = data;
	g_autoptr(GArray) hits_b = NULL;
	g_autoptr(GArray) hits_e = NULL;

	if (self == NULL || g_strcmp0(item_id, "search") != 0)
		return;

	hits_b = g_array_new(FALSE, FALSE, sizeof(guint));
	hits_e = g_array_new(FALSE, FALSE, sizeof(guint));

	g_mutex_lock(&self->lock);
	g_free(self->query);
	self->query = (text != NULL) ? g_utf8_make_valid(text, -1) : NULL;
	if (submitted)
		bar_keywheel_search(self->model, self->query, 1, hits_b,
		                    hits_e);
	g_mutex_unlock(&self->lock);

	if (hits_b->len > 0)
		kw_run_index(plugin, self, g_array_index(hits_b, guint, 0));
	else if (hits_e->len > 0)
		kw_run_emacs(plugin, self, g_array_index(hits_e, guint, 0));
}

/*
 * The wheel's keys; see bar_keywheel_handle_key() for the map.  `/'
 * cannot move the host's keyboard focus into the search field from
 * here (there is no call for it), so it closes the panel and opens it
 * again with the field marked AUTOFOCUS -- which the host does honour
 * on open.  `reopening' keeps the close from resetting the wheel.
 */
static gboolean
kw_panel_key(GowlBarPlugin *plugin, gpointer data, guint keysym,
             guint modifiers, gint focused_item)
{
	Keywheel *self = data;
	BarKeywheelKeyResult r;
	GowlBarHost *host;

	(void)focused_item;
	if (self == NULL)
		return FALSE;

	g_mutex_lock(&self->lock);
	r = bar_keywheel_handle_key(self->model, &self->cursor, keysym,
	                            modifiers);
	if (r == BAR_KEYWHEEL_KEY_SEARCH) {
		self->focus_search = TRUE;
		self->reopening = TRUE;
	}
	g_mutex_unlock(&self->lock);

	switch (r) {
	case BAR_KEYWHEEL_KEY_IGNORED:
		return FALSE;
	case BAR_KEYWHEEL_KEY_MOVED:
		gowl_bar_plugin_request_panel_refresh(plugin);
		return TRUE;
	case BAR_KEYWHEEL_KEY_SEARCH:
		host = gowl_bar_plugin_get_host(plugin);
		if (host != NULL) {
			gowl_bar_host_close_panel(host);
			gowl_bar_host_open_panel(host,
				gowl_bar_plugin_get_id(plugin));
		}
		g_mutex_lock(&self->lock);
		self->reopening = FALSE;
		g_mutex_unlock(&self->lock);
		return TRUE;
	case BAR_KEYWHEEL_KEY_CLAIMED:
	default:
		return TRUE;
	}
}

/* ----------------------------------------------------------------
 * Painting
 * ---------------------------------------------------------------- */

/* The wheel's colours, each a `color-*' setting over a role. */
enum {
	KW_C_SUPER, KW_C_SHIFT, KW_C_CTRL, KW_C_HUB, KW_C_ALT,
	KW_C_SELECTED, KW_C_TEXT, KW_C_MUTED, KW_C_BG, KW_C_UNBOUND,
	KW_C_COUNT
};

static const struct {
	const gchar *key;
	GowlBarColor role;
} kw_colors[KW_C_COUNT] = {
	{ "color-ring-super", GOWL_BAR_COLOR_BLUE },
	{ "color-ring-shift", GOWL_BAR_COLOR_MAUVE },
	{ "color-ring-ctrl",  GOWL_BAR_COLOR_PEACH },
	{ "color-ring-hub",   GOWL_BAR_COLOR_RED },
	{ "color-ring-alt",   GOWL_BAR_COLOR_TEAL },
	{ "color-selected",   GOWL_BAR_COLOR_ACCENT },
	{ "color-text",       GOWL_BAR_COLOR_TEXT },
	{ "color-muted",      GOWL_BAR_COLOR_MUTED },
	{ "color-bg",         GOWL_BAR_COLOR_MANTLE },
	{ "color-unbound",    GOWL_BAR_COLOR_SURFACE }
};

/* Resolve every colour once per paint: role names follow the session
   palette through @theme, literals are used as written. */
static void
kw_resolve_colors(GowlBarPlugin *plugin, const GowlBarTheme *theme,
                  gdouble out[KW_C_COUNT][4])
{
	guint i;

	for (i = 0; i < KW_C_COUNT; i++) {
		gint role;
		gdouble rgba[4];

		if (bar_keywheel_color_resolve(
		        gowl_bar_plugin_get_setting(plugin, kw_colors[i].key),
		        (gint)kw_colors[i].role, &role, rgba)
		    == BAR_KEYWHEEL_COLOR_HEX) {
			memcpy(out[i], rgba, sizeof(rgba));
			continue;
		}
		memcpy(out[i], gowl_bar_theme_color(theme, (GowlBarColor)role),
		       sizeof(gdouble) * 4);
	}
}

static void
kw_set(cairo_t *cr, const gdouble c[4], gdouble alpha)
{
	cairo_set_source_rgba(cr, c[0], c[1], c[2], c[3] * alpha);
}

/* Draw @text centred on (@x, @y), at @scale of the theme font. */
static void
kw_text(cairo_t *cr, PangoLayout *layout, const PangoFontDescription *base,
        const gchar *text, gdouble x, gdouble y, gdouble scale,
        gboolean bold, gint max_w)
{
	PangoFontDescription *font;
	PangoRectangle r;
	gint size;

	if (text == NULL || text[0] == '\0')
		return;
	font = pango_font_description_copy(base);
	size = pango_font_description_get_size(font);
	if (size <= 0)
		size = 10 * PANGO_SCALE;
	pango_font_description_set_size(font, (gint)((gdouble)size * scale));
	if (bold)
		pango_font_description_set_weight(font, PANGO_WEIGHT_BOLD);
	pango_layout_set_font_description(layout, font);
	pango_font_description_free(font);
	pango_layout_set_width(layout, (max_w > 0) ? max_w * PANGO_SCALE : -1);
	pango_layout_set_ellipsize(layout, (max_w > 0) ? PANGO_ELLIPSIZE_END
	                                               : PANGO_ELLIPSIZE_NONE);
	pango_layout_set_text(layout, text, -1);
	pango_layout_get_pixel_extents(layout, NULL, &r);
	cairo_move_to(cr, x - (gdouble)r.width / 2.0 - (gdouble)r.x,
	              y - (gdouble)r.height / 2.0 - (gdouble)r.y);
	pango_cairo_show_layout(cr, layout);
}

/* Draw @text from its left edge, vertically centred on @y. */
static void
kw_text_left(cairo_t *cr, PangoLayout *layout,
             const PangoFontDescription *base, const gchar *text, gdouble x,
             gdouble y, gdouble scale, gboolean bold, gint max_w)
{
	PangoRectangle r;

	if (text == NULL || text[0] == '\0')
		return;
	kw_text(cr, layout, base, text, -10000.0, -10000.0, scale, bold,
	        max_w);
	pango_layout_get_pixel_extents(layout, NULL, &r);
	cairo_move_to(cr, x - (gdouble)r.x,
	              y - (gdouble)r.height / 2.0 - (gdouble)r.y);
	pango_cairo_show_layout(cr, layout);
}

/* One annular sector, shrunk a little on every side so cells read as
   cells.  The hit test uses the unshrunk sector, so the gaps still
   belong to their cell. */
static void
kw_sector(cairo_t *cr, const BarKeywheelGeom *g, guint spoke,
          BarKeywheelRing ring)
{
	gdouble a0, a1, pad_a, rin, rout;

	bar_keywheel_spoke_span(g, spoke, &a0, &a1);
	rin  = g->ring_in[ring] + 1.0;
	rout = g->ring_out[ring] - 1.0;
	pad_a = 1.5 / MAX(rout, 1.0);
	cairo_new_path(cr);
	cairo_arc(cr, g->cx, g->cy, rout, a0 + pad_a, a1 - pad_a);
	cairo_arc_negative(cr, g->cx, g->cy, rin, a1 - pad_a, a0 + pad_a);
	cairo_close_path(cr);
}

static const gchar *
kw_ring_name(BarKeywheelRing ring)
{
	switch (ring) {
	case BAR_KEYWHEEL_RING_SHIFT: return "+Shift";
	case BAR_KEYWHEEL_RING_CTRL:  return "+Ctrl";
	case BAR_KEYWHEEL_RING_HUB:   return "+Shift+Ctrl";
	case BAR_KEYWHEEL_RING_SUPER:
	case BAR_KEYWHEEL_RING_COUNT:
	default:                      return "Super";
	}
}

/* The details box: the selection, or how to drive the wheel. */
static void
kw_paint_details(cairo_t *cr, PangoLayout *layout,
                 const PangoFontDescription *font, const BarKeywheelGeom *g,
                 const BarKeywheelModel *m, const BarKeywheelCursor *c,
                 gdouble col[KW_C_COUNT][4])
{
	const BarKeywheelBind *b = NULL;
	g_autofree gchar *line1 = NULL;
	g_autofree gchar *line2 = NULL;
	gdouble x, y, lx;
	gint maxw;
	guint i;

	x = g->details_x;
	y = g->details_y;
	maxw = (gint)(g->details_w - 24.0);

	kw_set(cr, col[KW_C_UNBOUND], 0.45);
	gowl_bar_cairo_rounded_rect(cr, x, y, g->details_w, g->details_h, 8.0);
	cairo_fill(cr);

	if (c->spoke >= 0) {
		const BarKeywheelSpoke *s;
		guint key;

		b = bar_keywheel_cell(m, c->face, (guint)c->spoke, c->ring,
		                      c->alt, c->keysym);
		s = bar_keywheel_spoke(m, c->face, (guint)c->spoke);
		key = (c->keysym != 0) ? c->keysym
		    : (s != NULL ? s->keysyms[0] : 0);
		if (b != NULL) {
			g_autofree gchar *what = kw_bind_title(b);

			line1 = g_strdup(b->combo);
			line2 = (b->flags & BAR_KEYWHEEL_FLAG_RELEASE)
				? g_strdup_printf("%s  (on release)", what)
				: g_steal_pointer(&what);
		} else {
			line1 = bar_keywheel_combo(
				bar_keywheel_ring_mods(c->ring, c->alt), key);
			line2 = g_strdup("Free: nothing is bound here.");
		}
	} else {
		line1 = g_strdup("Type a key to find it");
		line2 = g_strdup("Shift Ctrl Alt: ring  \xc2\xb7  "
		                 "\xe2\x86\x90 \xe2\x86\x92 Home End: move  "
		                 "\xc2\xb7  PgUp PgDn: face  \xc2\xb7  /: search");
	}

	kw_set(cr, col[KW_C_TEXT], 1.0);
	kw_text_left(cr, layout, font, line1, x + 12.0, y + 16.0, 1.05, TRUE,
	             maxw);
	kw_set(cr, (b != NULL || c->spoke < 0) ? col[KW_C_TEXT]
	                                       : col[KW_C_MUTED], 1.0);
	kw_text_left(cr, layout, font, line2, x + 12.0, y + 38.0,
	             (c->spoke < 0) ? 0.85 : 0.95, FALSE, maxw);

	/* The legend: which colour is which ring. */
	lx = x + 12.0;
	for (i = 0; i < BAR_KEYWHEEL_RING_COUNT; i++) {
		PangoRectangle r;
		const gchar *name = kw_ring_name((BarKeywheelRing)i);

		kw_set(cr, c->alt ? col[KW_C_ALT] : col[KW_C_SUPER + i],
		       c->alt ? 1.0 - 0.15 * (gdouble)i : 1.0);
		cairo_arc(cr, lx + 5.0, y + 62.0, 5.0, 0.0, 2.0 * G_PI);
		cairo_fill(cr);
		kw_set(cr, col[KW_C_MUTED], 1.0);
		kw_text_left(cr, layout, font, name, lx + 14.0, y + 62.0, 0.85,
		             FALSE, -1);
		pango_layout_get_pixel_extents(layout, NULL, &r);
		lx += 14.0 + (gdouble)r.width + 16.0;
	}
	if (c->alt) {
		kw_set(cr, col[KW_C_ALT], 1.0);
		kw_text_left(cr, layout, font, "all +Alt", lx, y + 62.0, 0.85,
		             TRUE, -1);
	}
}

/* Header: face title, binding count, the Alt switch. */
static void
kw_paint_header(cairo_t *cr, PangoLayout *layout,
                const PangoFontDescription *font, const BarKeywheelGeom *g,
                const BarKeywheelModel *m, const BarKeywheelCursor *c,
                gdouble col[KW_C_COUNT][4])
{
	g_autofree gchar *title = NULL;

	if (c->face == BAR_KEYWHEEL_FACE_KEYS && m != NULL &&
	    m->keys_overflow > 0)
		title = g_strdup_printf("%s  (+%u in search)",
			bar_keywheel_face_title(c->face), m->keys_overflow);
	else
		title = g_strdup(bar_keywheel_face_title(c->face));
	kw_set(cr, col[KW_C_TEXT], 1.0);
	kw_text(cr, layout, font, title, g->width / 2.0, g->header_h / 2.0,
	        1.15, TRUE, (gint)(g->width - 2.0 * (g->alt_w + 16.0)));

	kw_set(cr, col[KW_C_MUTED], 1.0);
	if (m != NULL) {
		g_autofree gchar *count = g_strdup_printf("%u binds",
		                                          m->binds->len);

		kw_text_left(cr, layout, font, count, 8.0, g->header_h / 2.0,
		             0.85, FALSE, -1);
	}

	/* The Alt pill: filled when the wheel is flipped. */
	gowl_bar_cairo_rounded_rect(cr, g->alt_x, g->alt_y, g->alt_w, g->alt_h,
	                            g->alt_h / 2.0);
	if (c->alt) {
		kw_set(cr, col[KW_C_ALT], 1.0);
		cairo_fill(cr);
		kw_set(cr, col[KW_C_BG], 1.0);
	} else {
		kw_set(cr, col[KW_C_MUTED], 1.0);
		cairo_set_line_width(cr, 1.0);
		cairo_stroke(cr);
		kw_set(cr, col[KW_C_MUTED], 1.0);
	}
	kw_text(cr, layout, font, "Alt", g->alt_x + g->alt_w / 2.0,
	        g->alt_y + g->alt_h / 2.0, 0.85, TRUE, -1);
}

static void
kw_paint_wheel(cairo_t *cr, PangoLayout *layout,
               const PangoFontDescription *font, const BarKeywheelGeom *g,
               const BarKeywheelModel *m, const BarKeywheelCursor *c,
               gdouble col[KW_C_COUNT][4])
{
	guint s, r;
	gdouble step;

	step = 2.0 * G_PI / (gdouble)MAX(g->n_spokes, 1u);
	for (r = 0; r < BAR_KEYWHEEL_RING_COUNT; r++) {
		gdouble mid_r = (g->ring_in[r] + g->ring_out[r]) / 2.0;
		gboolean room = mid_r * step >= 22.0;

		for (s = 0; s < g->n_spokes; s++) {
			const BarKeywheelBind *b;
			const BarKeywheelSpoke *sp;
			gboolean sel;
			gdouble cx, cy;

			b = bar_keywheel_cell(m, c->face, s, (BarKeywheelRing)r,
			                      c->alt, 0);
			sp = bar_keywheel_spoke(m, c->face, s);
			sel = c->spoke == (gint)s && c->ring == (BarKeywheelRing)r;

			kw_sector(cr, g, s, (BarKeywheelRing)r);
			if (b != NULL && c->alt)
				kw_set(cr, col[KW_C_ALT], 1.0 - 0.15 * (gdouble)r);
			else if (b != NULL)
				kw_set(cr, col[KW_C_SUPER + r], 1.0);
			else
				kw_set(cr, col[KW_C_UNBOUND], 1.0);
			cairo_fill_preserve(cr);
			if (sel) {
				kw_set(cr, col[KW_C_SELECTED], 1.0);
				cairo_set_line_width(cr, 3.0);
				cairo_stroke(cr);
			} else {
				cairo_new_path(cr);
			}

			/* Labels: always on the rim; inside only where there
			   is room and something to name. */
			if (sp == NULL || (r != BAR_KEYWHEEL_RING_SUPER &&
			                   (!room || b == NULL)))
				continue;
			bar_keywheel_cell_center(g, s, (BarKeywheelRing)r,
			                         &cx, &cy);
			kw_set(cr, (b != NULL) ? col[KW_C_BG] : col[KW_C_MUTED],
			       1.0);
			kw_text(cr, layout, font, sp->label, cx, cy,
			        (r == BAR_KEYWHEEL_RING_SUPER) ? 0.95 : 0.75,
			        b != NULL, (gint)(mid_r * step));
		}
	}

	/* The centre: the Alt switch, as the hub of the wheel. */
	cairo_new_path(cr);
	cairo_arc(cr, g->cx, g->cy, g->hub_r, 0.0, 2.0 * G_PI);
	kw_set(cr, c->alt ? col[KW_C_ALT] : col[KW_C_UNBOUND], 1.0);
	cairo_fill(cr);
	kw_set(cr, c->alt ? col[KW_C_BG] : col[KW_C_MUTED], 1.0);
	kw_text(cr, layout, font, "Alt", g->cx, g->cy, 0.8, c->alt, -1);
}

static void
kw_canvas(GowlBarPlugin *plugin, gpointer data, const gchar *item_id,
          cairo_t *cr, const GowlBarTheme *theme, gint width, gint height)
{
	Keywheel *self = data;
	gdouble col[KW_C_COUNT][4];
	BarKeywheelGeom g;
	PangoLayout *layout;
	PangoFontDescription *font;
	const gchar *font_name;
	guint f, n_faces;
	gboolean wheel;

	if (self == NULL || g_strcmp0(item_id, "wheel") != 0 || theme == NULL)
		return;

	kw_resolve_colors(plugin, theme, col);
	font_name = gowl_bar_theme_get_font(theme);
	font = pango_font_description_from_string(
		(font_name != NULL) ? font_name : "Sans 10");
	layout = pango_cairo_create_layout(cr);

	g_mutex_lock(&self->lock);
	self->painted_w = width;
	self->painted_h = height;
	n_faces = bar_keywheel_face_count(self->model);
	wheel = bar_keywheel_face_is_wheel(self->cursor.face) &&
	        bar_keywheel_n_spokes(self->model, self->cursor.face) > 0;
	bar_keywheel_geom((gdouble)width, (gdouble)height,
	                  bar_keywheel_n_spokes(self->model, self->cursor.face),
	                  n_faces, wheel, &g);

	kw_set(cr, col[KW_C_BG], 1.0);
	gowl_bar_cairo_rounded_rect(cr, 0.0, 0.0, (gdouble)width,
	                            (gdouble)height, 10.0);
	cairo_fill(cr);

	kw_paint_header(cr, layout, font, &g, self->model, &self->cursor, col);
	if (wheel) {
		kw_paint_wheel(cr, layout, font, &g, self->model, &self->cursor,
		               col);
		kw_paint_details(cr, layout, font, &g, self->model,
		                 &self->cursor, col);
	}

	for (f = 0; f < n_faces; f++) {
		gdouble x, y;

		bar_keywheel_dot_center(&g, f, &x, &y);
		cairo_new_path(cr);
		cairo_arc(cr, x, y, (f == (guint)self->cursor.face)
		                    ? g.dot_r + 1.5 : g.dot_r, 0.0, 2.0 * G_PI);
		kw_set(cr, (f == (guint)self->cursor.face)
		           ? col[KW_C_SELECTED] : col[KW_C_MUTED], 1.0);
		cairo_fill(cr);
	}
	g_mutex_unlock(&self->lock);

	g_object_unref(layout);
	pango_font_description_free(font);
}

/* ----------------------------------------------------------------
 * Registration
 * ---------------------------------------------------------------- */

static const GowlBarPluginVTable keywheel_vtable = {
	sizeof(GowlBarPluginVTable),
	kw_create, kw_destroy,
	NULL, NULL, kw_configure,
	kw_interval, kw_poll, NULL,
	NULL, NULL,
	NULL, NULL,
	kw_panel, kw_action,
	kw_panel_opened, kw_panel_closed,
	kw_panel_key,
	NULL,
	kw_panel_text, kw_canvas
};

/**
 * bar_register_keywheel_plugins:
 * @registry: the bar's plugin registry
 *
 * Registers `keywheel', with the aliases `keybinds' and `cheatsheet'.
 */
void
bar_register_keywheel_plugins(GowlBarRegistry *registry)
{
	gowl_bar_registry_register_vtable(registry, "keywheel", "Key wheel",
		"A radial cheat sheet of every keybinding", &keywheel_vtable);
	gowl_bar_registry_register_alias(registry, "keybinds", "keywheel");
	gowl_bar_registry_register_alias(registry, "cheatsheet", "keywheel");
}
