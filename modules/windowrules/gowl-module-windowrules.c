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

#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "gowl-windowrules"

#include <glib-object.h>
#include <gmodule.h>
#include <string.h>
#include <stdlib.h>

#include "module/gowl-module.h"
#include "interfaces/gowl-startup-handler.h"
#include "interfaces/gowl-keybind-handler.h"
#include "config/gowl-config.h"
#include "config/gowl-keybind.h"
#include "core/gowl-compositor.h"
#include "core/gowl-client.h"
#include "core/gowl-monitor.h"

/**
 * GowlModuleWindowrules:
 *
 * Applies #GowlRuleEntry entries from the gowl config to newly
 * mapped clients, and provides keybinds for managing floating
 * windows at runtime.
 *
 * The module connects to the compositor's %client-pre-map signal
 * (emitted before the new client is assigned a monitor) and
 * walks @gowl_config_get_rules(), applying the first matching
 * rule.  A matched rule may set the client's floating state,
 * target tag bitmask, target monitor, and explicit geometry.
 *
 * After map, the module also connects each client's "set-title"
 * signal so that apps which update their title post-map (for
 * example Zoom showing a "Join Meeting" window) still get their
 * title-based rule applied.
 *
 * Keybinds are read from the `windowrules:` module config
 * section in config.yaml.  Defaults (if the section is absent)
 * are:
 *
 *   toggle-float:       Super+space
 *   center-float:       Super+c
 *   move-float-left:    Super+Shift+Left
 *   move-float-right:   Super+Shift+Right
 *   move-float-up:      Super+Shift+Up
 *   move-float-down:    Super+Shift+Down
 *   grow-float-w:       Super+Ctrl+Right
 *   shrink-float-w:     Super+Ctrl+Left
 *   grow-float-h:       Super+Ctrl+Down
 *   shrink-float-h:     Super+Ctrl+Up
 */

#define GOWL_TYPE_MODULE_WINDOWRULES (gowl_module_windowrules_get_type())
G_DECLARE_FINAL_TYPE(GowlModuleWindowrules, gowl_module_windowrules,
                     GOWL, MODULE_WINDOWRULES, GowlModule)

/**
 * WRActionKind:
 *
 * Enumerates the float-management actions the module can bind
 * to keys.  Used as the index into the @binds array.
 */
typedef enum {
	WR_TOGGLE_FLOAT = 0,
	WR_CENTER_FLOAT,
	WR_MOVE_LEFT,
	WR_MOVE_RIGHT,
	WR_MOVE_UP,
	WR_MOVE_DOWN,
	WR_GROW_W,
	WR_SHRINK_W,
	WR_GROW_H,
	WR_SHRINK_H,
	WR_ACTION_COUNT
} WRActionKind;

/**
 * WRBind:
 * @active: %TRUE if the bind has been configured
 * @modifiers: modifier bitmask
 * @keysym: XKB keysym
 *
 * Parsed keybind entry for one action.
 */
typedef struct {
	gboolean active;
	guint    modifiers;
	guint    keysym;
} WRBind;

struct _GowlModuleWindowrules {
	GowlModule       parent_instance;
	GowlCompositor  *compositor;
	gulong           pre_map_handler_id;
	WRBind           binds[WR_ACTION_COUNT];

	/* Step pixels for move/resize — configurable via YAML, default
	 * 32 pixels per key press. */
	gint             step_px;
};

static void wr_startup_init(GowlStartupHandlerInterface *iface);
static void wr_keybind_init(GowlKeybindHandlerInterface *iface);

G_DEFINE_TYPE_WITH_CODE(GowlModuleWindowrules, gowl_module_windowrules,
	GOWL_TYPE_MODULE,
	G_IMPLEMENT_INTERFACE(GOWL_TYPE_STARTUP_HANDLER,
		wr_startup_init)
	G_IMPLEMENT_INTERFACE(GOWL_TYPE_KEYBIND_HANDLER,
		wr_keybind_init))

/* Forward declarations */
static void wr_apply_rules_to_client(GowlModuleWindowrules *self,
                                      GowlClient            *c);
static void wr_on_client_pre_map(GowlCompositor *compositor,
                                  GowlClient     *c,
                                  gpointer        user_data);
static void wr_on_client_set_title(GowlClient  *c,
                                    const gchar *title,
                                    gpointer     user_data);

/* --- Helpers --- */

/**
 * wr_rule_entry_matches:
 * @rule: a rule entry from the config
 * @app_id: (nullable): the client's app_id
 * @title: (nullable): the client's title
 *
 * Tests a config #GowlRuleEntry against client identity strings.
 * The entry stores its pattern strings plus a @regex_mode flag;
 * we compile the regex on the fly per-match (rules are iterated
 * infrequently, so we prioritise correctness over caching here).
 * Glob mode uses g_pattern_match_simple().
 *
 * Returns: %TRUE if the client matches
 */
static gboolean
wr_rule_entry_matches(
	GowlRuleEntry *rule,
	const gchar   *app_id,
	const gchar   *title
){
	gboolean app_id_ok;
	gboolean title_ok;

	app_id_ok = TRUE;
	title_ok = TRUE;

	if (rule->app_id != NULL) {
		if (app_id == NULL)
			return FALSE;
		if (rule->regex_mode) {
			GRegex *rx = g_regex_new(rule->app_id,
			                          G_REGEX_OPTIMIZE, 0, NULL);
			app_id_ok = (rx != NULL &&
			             g_regex_match(rx, app_id, 0, NULL));
			if (rx != NULL)
				g_regex_unref(rx);
		} else {
			app_id_ok = g_pattern_match_simple(rule->app_id, app_id);
		}
	}

	if (!app_id_ok)
		return FALSE;

	if (rule->title != NULL) {
		if (title == NULL)
			return FALSE;
		if (rule->regex_mode) {
			GRegex *rx = g_regex_new(rule->title,
			                          G_REGEX_OPTIMIZE, 0, NULL);
			title_ok = (rx != NULL &&
			            g_regex_match(rx, title, 0, NULL));
			if (rx != NULL)
				g_regex_unref(rx);
		} else {
			title_ok = g_pattern_match_simple(rule->title, title);
		}
	}

	return title_ok;
}

/**
 * wr_parse_bind:
 * @self: the module
 * @action: which action slot to populate
 * @str: the keybind string, e.g. "Super+space"
 *
 * Parses @str into modifier/keysym and stores into the
 * @binds[@action] slot.  Logs a warning on parse failure.
 */
static void
wr_parse_bind(
	GowlModuleWindowrules *self,
	WRActionKind           action,
	const gchar           *str
){
	guint mods;
	guint sym;

	if (str == NULL || str[0] == '\0')
		return;

	if (!gowl_keybind_parse(str, &mods, &sym)) {
		g_warning("windowrules: could not parse keybind '%s'", str);
		return;
	}

	self->binds[action].active    = TRUE;
	self->binds[action].modifiers = mods;
	self->binds[action].keysym    = sym;
	g_debug("windowrules: bound %s -> action %d", str, (gint)action);
}

/**
 * wr_apply_defaults:
 * @self: the module
 *
 * Populates any unset binds with the documented defaults.
 */
static void
wr_apply_defaults(GowlModuleWindowrules *self)
{
	static const gchar *defaults[WR_ACTION_COUNT] = {
		"Super+space",         /* WR_TOGGLE_FLOAT */
		"Super+c",             /* WR_CENTER_FLOAT */
		"Super+Shift+Left",    /* WR_MOVE_LEFT */
		"Super+Shift+Right",   /* WR_MOVE_RIGHT */
		"Super+Shift+Up",      /* WR_MOVE_UP */
		"Super+Shift+Down",    /* WR_MOVE_DOWN */
		"Super+Ctrl+Right",    /* WR_GROW_W */
		"Super+Ctrl+Left",     /* WR_SHRINK_W */
		"Super+Ctrl+Down",     /* WR_GROW_H */
		"Super+Ctrl+Up"        /* WR_SHRINK_H */
	};
	gint i;

	for (i = 0; i < WR_ACTION_COUNT; i++) {
		if (!self->binds[i].active)
			wr_parse_bind(self, (WRActionKind)i, defaults[i]);
	}
}

/* --- GowlModule virtual methods --- */

static gboolean
wr_activate(GowlModule *mod)
{
	(void)mod;
	return TRUE;
}

static void
wr_deactivate(GowlModule *mod)
{
	GowlModuleWindowrules *self;

	self = GOWL_MODULE_WINDOWRULES(mod);
	if (self->compositor != NULL && self->pre_map_handler_id != 0) {
		g_signal_handler_disconnect(self->compositor,
		                             self->pre_map_handler_id);
		self->pre_map_handler_id = 0;
	}
	self->compositor = NULL;
}

static const gchar *
wr_get_name(GowlModule *mod)
{
	(void)mod;
	return "windowrules";
}

static const gchar *
wr_get_description(GowlModule *mod)
{
	(void)mod;
	return "Apply float/tag/monitor rules and float-management keybinds";
}

static const gchar *
wr_get_version(GowlModule *mod)
{
	(void)mod;
	return "0.1.0";
}

/**
 * wr_configure:
 * @mod: the module
 * @config: (nullable): per-module GHashTable settings
 *
 * Reads keybind entries and step-pixel count from the YAML
 * module config.  Unset fields fall back to defaults applied in
 * wr_on_startup().
 */
static void
wr_configure(GowlModule *mod, gpointer config)
{
	GowlModuleWindowrules *self;
	GHashTable *settings;
	const gchar *val;

	self = GOWL_MODULE_WINDOWRULES(mod);
	if (config == NULL)
		return;

	settings = (GHashTable *)config;

	val = (const gchar *)g_hash_table_lookup(settings, "toggle-float");
	wr_parse_bind(self, WR_TOGGLE_FLOAT, val);
	val = (const gchar *)g_hash_table_lookup(settings, "center-float");
	wr_parse_bind(self, WR_CENTER_FLOAT, val);
	val = (const gchar *)g_hash_table_lookup(settings, "move-float-left");
	wr_parse_bind(self, WR_MOVE_LEFT, val);
	val = (const gchar *)g_hash_table_lookup(settings, "move-float-right");
	wr_parse_bind(self, WR_MOVE_RIGHT, val);
	val = (const gchar *)g_hash_table_lookup(settings, "move-float-up");
	wr_parse_bind(self, WR_MOVE_UP, val);
	val = (const gchar *)g_hash_table_lookup(settings, "move-float-down");
	wr_parse_bind(self, WR_MOVE_DOWN, val);
	val = (const gchar *)g_hash_table_lookup(settings, "grow-float-w");
	wr_parse_bind(self, WR_GROW_W, val);
	val = (const gchar *)g_hash_table_lookup(settings, "shrink-float-w");
	wr_parse_bind(self, WR_SHRINK_W, val);
	val = (const gchar *)g_hash_table_lookup(settings, "grow-float-h");
	wr_parse_bind(self, WR_GROW_H, val);
	val = (const gchar *)g_hash_table_lookup(settings, "shrink-float-h");
	wr_parse_bind(self, WR_SHRINK_H, val);

	val = (const gchar *)g_hash_table_lookup(settings, "step-px");
	if (val != NULL) {
		gint v = atoi(val);
		if (v > 0)
			self->step_px = v;
	}
}

/* --- Rule application --- */

/**
 * wr_apply_rules_to_client:
 * @self: the module
 * @c: the client to evaluate
 *
 * Iterates the config's rule array and applies the first rule
 * whose patterns match the client's app_id and title.  This is
 * invoked both from the pre-map signal handler and from the
 * per-client set-title signal handler (post-map re-evaluation).
 */
static void
wr_apply_rules_to_client(
	GowlModuleWindowrules *self,
	GowlClient            *c
){
	GowlConfig  *cfg;
	GPtrArray   *rules;
	const gchar *app_id;
	const gchar *title;
	guint        i;

	if (self->compositor == NULL || c == NULL)
		return;

	cfg = gowl_compositor_get_config(self->compositor);
	if (cfg == NULL)
		return;

	rules = gowl_config_get_rules(cfg);
	if (rules == NULL || rules->len == 0)
		return;

	app_id = gowl_client_get_app_id(c);
	title  = gowl_client_get_title(c);

	for (i = 0; i < rules->len; i++) {
		GowlRuleEntry *rule;

		rule = (GowlRuleEntry *)g_ptr_array_index(rules, i);
		if (!wr_rule_entry_matches(rule, app_id, title))
			continue;

		g_debug("windowrules: match app_id=%s title=%s "
		        "float=%d tags=0x%x mon=%d",
		        app_id ? app_id : "(null)",
		        title  ? title  : "(null)",
		        (gint)rule->floating,
		        (guint)rule->tags,
		        rule->monitor);

		if (rule->floating) {
			if (!gowl_client_get_floating(c))
				gowl_client_set_floating(c, TRUE);
		}

		/* Stash initial-placement overrides via the public
		 * setter; the compositor consumes these inside
		 * on_client_map() after client-pre-map returns.
		 * Post-map invocations (from set-title) still write
		 * the fields but the compositor no longer reads
		 * them — the tag/monitor components of a late match
		 * are best-effort; the floating flip is live via
		 * gowl_client_set_floating() above. */
		{
			gboolean geom_set = FALSE;

			if (rule->floating &&
			    (rule->width > 0 || rule->height > 0)) {
				gint gx, gy, gw, gh;

				gowl_client_get_geometry(c, &gx, &gy, &gw, &gh);
				if (rule->width > 0)
					gw = rule->width;
				if (rule->height > 0)
					gh = rule->height;
				gowl_client_set_geometry(c, gx, gy, gw, gh);
				geom_set = TRUE;
			}

			gowl_client_set_rule_overrides(
				c,
				(rule->tags != 0) ? rule->tags : 0,
				(rule->monitor >= 0) ? rule->monitor : -1,
				geom_set);
		}

		return; /* first match wins */
	}
}

/* --- GowlStartupHandler --- */

/**
 * wr_on_client_pre_map:
 * @compositor: the compositor
 * @c: the newly created client
 * @user_data: GowlModuleWindowrules *
 *
 * Signal handler invoked by the compositor just before a new
 * client is placed.  Applies matching rules and subscribes to
 * the client's set-title signal for post-map re-evaluation.
 */
static void
wr_on_client_pre_map(
	GowlCompositor *compositor,
	GowlClient     *c,
	gpointer        user_data
){
	GowlModuleWindowrules *self;

	(void)compositor;
	self = (GowlModuleWindowrules *)user_data;

	wr_apply_rules_to_client(self, c);

	/* Attach a set-title listener so rules matching on title can
	 * still apply after a post-map rename.  g_signal_connect does
	 * not take a ref on user_data; disconnect happens
	 * automatically when the client is destroyed. */
	g_signal_connect(c, "set-title",
	                 G_CALLBACK(wr_on_client_set_title), self);
}

/**
 * wr_on_client_set_title:
 * @c: the client whose title changed
 * @title: the new title string
 * @user_data: GowlModuleWindowrules *
 *
 * Re-runs rule matching when a client's title changes.  The
 * floating flip takes effect immediately via
 * gowl_client_set_floating() which the compositor implements as
 * an arrange-friendly state change.  Tag/monitor overrides
 * stored in pending_rule_* are best-effort (no re-placement
 * path exists after the initial setmon).
 */
static void
wr_on_client_set_title(
	GowlClient  *c,
	const gchar *title,
	gpointer     user_data
){
	GowlModuleWindowrules *self;

	(void)title;
	self = (GowlModuleWindowrules *)user_data;
	wr_apply_rules_to_client(self, c);
}

static void
wr_on_startup(GowlStartupHandler *handler, gpointer compositor)
{
	GowlModuleWindowrules *self;

	self = GOWL_MODULE_WINDOWRULES(handler);
	self->compositor = GOWL_COMPOSITOR(compositor);

	wr_apply_defaults(self);

	self->pre_map_handler_id = g_signal_connect(
		self->compositor, "client-pre-map",
		G_CALLBACK(wr_on_client_pre_map), self);

	g_debug("windowrules: startup, compositor=%p, %d default binds",
	        (gpointer)self->compositor, WR_ACTION_COUNT);
}

static void
wr_startup_init(GowlStartupHandlerInterface *iface)
{
	iface->on_startup = wr_on_startup;
}

/* --- GowlKeybindHandler --- */

/**
 * wr_find_action:
 * @self: the module
 * @modifiers: modifier bitmask from the key event
 * @keysym: keysym from the key event
 *
 * Linear scan of the binds array to find which action (if any)
 * matches the key event.  There are only 10 actions, so this is
 * cheaper than a hash lookup.
 *
 * Returns: the action index, or -1 if no match
 */
static gint
wr_find_action(
	GowlModuleWindowrules *self,
	guint                  modifiers,
	guint                  keysym
){
	gint i;

	for (i = 0; i < WR_ACTION_COUNT; i++) {
		if (!self->binds[i].active)
			continue;
		if (self->binds[i].modifiers == modifiers &&
		    self->binds[i].keysym == keysym)
			return i;
	}
	return -1;
}

/**
 * wr_dispatch_action:
 * @self: the module
 * @action: which float action to perform
 *
 * Performs the action on the currently focused client.  If no
 * client is focused, the action is a no-op.  Non-floating
 * clients are ignored for move/resize actions but can still be
 * toggled via WR_TOGGLE_FLOAT.
 */
static void
wr_dispatch_action(
	GowlModuleWindowrules *self,
	WRActionKind           action
){
	GowlClient *c;
	gint        x, y, w, h;
	gint        step;

	if (self->compositor == NULL)
		return;
	c = gowl_compositor_get_focused_client(self->compositor);
	if (c == NULL)
		return;

	step = self->step_px > 0 ? self->step_px : 32;

	if (action == WR_TOGGLE_FLOAT) {
		gboolean was_floating;

		was_floating = gowl_client_get_floating(c);
		gowl_client_set_floating(c, !was_floating);
		return;
	}

	/* Remaining actions only apply to floating clients. */
	if (!gowl_client_get_floating(c))
		return;

	gowl_client_get_geometry(c, &x, &y, &w, &h);

	switch (action) {
	case WR_CENTER_FLOAT: {
		gpointer mptr = gowl_client_get_monitor(c);
		if (mptr != NULL) {
			gint mx, my, mw, mh;
			gowl_monitor_get_window_area((GowlMonitor *)mptr,
			                              &mx, &my, &mw, &mh);
			x = mx + (mw - w) / 2;
			y = my + (mh - h) / 2;
		}
		break;
	}
	case WR_MOVE_LEFT:   x -= step; break;
	case WR_MOVE_RIGHT:  x += step; break;
	case WR_MOVE_UP:     y -= step; break;
	case WR_MOVE_DOWN:   y += step; break;
	case WR_GROW_W:      w += step; break;
	case WR_SHRINK_W:    if (w - step > 64) w -= step; break;
	case WR_GROW_H:      h += step; break;
	case WR_SHRINK_H:    if (h - step > 64) h -= step; break;
	default: return;
	}

	gowl_client_set_geometry(c, x, y, w, h);
}

static gboolean
wr_handle_key(
	GowlKeybindHandler *handler,
	guint               modifiers,
	guint               keysym,
	gboolean            pressed
){
	GowlModuleWindowrules *self;
	gint                   action;

	if (!pressed)
		return FALSE;

	self = GOWL_MODULE_WINDOWRULES(handler);
	action = wr_find_action(self, modifiers, keysym);
	if (action < 0)
		return FALSE;

	wr_dispatch_action(self, (WRActionKind)action);
	return TRUE;
}

static void
wr_keybind_init(GowlKeybindHandlerInterface *iface)
{
	iface->handle_key = wr_handle_key;
}

/* --- GObject lifecycle --- */

static void
gowl_module_windowrules_class_init(GowlModuleWindowrulesClass *klass)
{
	GowlModuleClass *mod_class;

	mod_class = GOWL_MODULE_CLASS(klass);

	mod_class->activate        = wr_activate;
	mod_class->deactivate      = wr_deactivate;
	mod_class->get_name        = wr_get_name;
	mod_class->get_description = wr_get_description;
	mod_class->get_version     = wr_get_version;
	mod_class->configure       = wr_configure;
}

static void
gowl_module_windowrules_init(GowlModuleWindowrules *self)
{
	gint i;

	self->compositor = NULL;
	self->pre_map_handler_id = 0;
	self->step_px = 32;
	for (i = 0; i < WR_ACTION_COUNT; i++) {
		self->binds[i].active = FALSE;
		self->binds[i].modifiers = 0;
		self->binds[i].keysym = 0;
	}
}

/* --- Shared-object entry point --- */

G_MODULE_EXPORT GType
gowl_module_register(void)
{
	return GOWL_TYPE_MODULE_WINDOWRULES;
}
