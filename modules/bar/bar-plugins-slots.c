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
#define G_LOG_DOMAIN "gowl-bar"

#include <string.h>

#include <cairo.h>
#include <pango/pangocairo.h>

#include "gowl-enums.h"
#include "core/gowl-compositor.h"
#include "core/gowl-monitor.h"

#include <linux/input-event-codes.h>

#include "bar-internal.h"
#include "bar-slots.h"

/**
 * SECTION:bar-plugins-slots
 * @title: The slot plugin
 * @short_description: draws one pushed bar item
 *
 * A slot is content somebody else pushes (see bar-slots.h).  This is
 * the widget that shows one: an ordinary vtable plugin, instantiated by
 * the host as `slot:ID', polled, measured, drawn and clicked through
 * exactly the entry points every other plugin has, and so under the
 * same fault guard and the same quarantine.
 *
 * It holds no content of its own.  The host writes the slot's fields
 * into the plugin's settings and calls configure -- the path every
 * `<widget>.<key>' setting has always taken -- and configure turns them
 * into a label, a glyph, a role and a tooltip.  The default drawing
 * does the rest.
 *
 * Three things the default cannot do, and so are done here:
 *
 *  - A slot may belong to ONE output.  One plugin object serves every
 *    screen, so it measures to nothing when it is asked about a screen
 *    that is not its own, and says so in its signature (or that screen
 *    would never repaint when the slot moved).
 *  - A critical slot is drawn over a wash of the red role, so it reads
 *    as urgent on a bar where red text alone is one more colour.
 *  - A click is REPORTED: `EVENT bar slot-click ...' to the IPC
 *    socket's subscribers and the compositor's `module-event' signal to
 *    an embedder.  Nothing is spawned and nothing the pusher sent is
 *    interpreted; what a click means is the owner's business, decided
 *    in the owner's process.
 */

/* What configure caches, so that measure and draw -- which run once per
   output per repaint -- do not look settings up. */
typedef struct {
	gchar          *output;     /* NULL: every output */
	BarSlotUrgency  urgency;
	gboolean        click;
} SlotData;

static gpointer
slot_create(
	GowlBarPlugin *plugin
){
	(void)plugin;
	return g_new0(SlotData, 1);
}

static void
slot_destroy(
	GowlBarPlugin *plugin,
	gpointer       data
){
	SlotData *sd = data;

	(void)plugin;
	if (sd == NULL)
		return;
	g_free(sd->output);
	g_free(sd);
}

static void
slot_configure(
	GowlBarPlugin *plugin,
	gpointer       data,
	GHashTable    *settings
){
	SlotData     *sd = data;
	const gchar  *val;
	GowlBarColor  role;

	(void)settings;

	val = gowl_bar_plugin_get_setting(plugin, "text");
	gowl_bar_plugin_set_label(plugin,
		(val != NULL && val[0] != '\0') ? val : NULL);

	val = gowl_bar_plugin_get_setting(plugin, "icon");
	gowl_bar_plugin_set_icon(plugin,
		(val != NULL && val[0] != '\0') ? val : NULL);

	/* With no hover text of its own a slot is named by its id, which
	   beats the plugin's title: every slot's title is "Slot". */
	val = gowl_bar_plugin_get_setting(plugin, "tooltip");
	if (val == NULL || val[0] == '\0')
		val = gowl_bar_plugin_get_setting(plugin, "param");
	gowl_bar_plugin_set_tooltip(plugin, val);

	val = gowl_bar_plugin_get_setting(plugin, "urgency");
	if (g_strcmp0(val, "low") == 0)
		sd->urgency = BAR_SLOT_LOW;
	else if (g_strcmp0(val, "critical") == 0)
		sd->urgency = BAR_SLOT_CRITICAL;
	else
		sd->urgency = BAR_SLOT_NORMAL;

	/*
	 * A role, by name, or the urgency's own.  Never a literal: the
	 * store refuses anything that is not a role before it gets here,
	 * and an unknown name -- a setting written by hand -- falls back
	 * rather than being parsed as a colour.
	 */
	val = gowl_bar_plugin_get_setting(plugin, "role");
	if (val == NULL || val[0] == '\0' ||
	    !gowl_bar_theme_color_from_name(val, &role)) {
		if (sd->urgency == BAR_SLOT_CRITICAL)
			role = GOWL_BAR_COLOR_RED;
		else if (sd->urgency == BAR_SLOT_LOW)
			role = GOWL_BAR_COLOR_MUTED;
		else
			role = GOWL_BAR_COLOR_TEXT;
	}
	gowl_bar_plugin_set_color(plugin, role);

	val = gowl_bar_plugin_get_setting(plugin, "output");
	g_free(sd->output);
	sd->output = (val != NULL && val[0] != '\0') ? g_strdup(val) : NULL;

	sd->click = gowl_bar_plugin_get_setting_bool(plugin, "click", FALSE);
}

/*
 * Is this slot on the output the current call is for?
 *
 * The served output, not the focused one: this runs in measure, draw
 * and signature, which the host calls once per screen.  With no served
 * output there is no one screen to answer about, and the slot counts
 * as shown.
 */
static gboolean
slot_shown(
	GowlBarPlugin  *plugin,
	const SlotData *sd
){
	gpointer mon;

	if (sd->output == NULL)
		return TRUE;
	mon = gowl_bar_plugin_get_monitor(plugin);
	if (mon == NULL)
		return TRUE;
	return g_strcmp0(gowl_monitor_get_name(GOWL_MONITOR(mon)),
	                 sd->output) == 0;
}

static gint
slot_measure(
	GowlBarPlugin      *plugin,
	gpointer            data,
	PangoLayout        *layout,
	const GowlBarTheme *theme,
	gint                height
){
	const SlotData *sd = data;

	/* Zero width is how a widget is absent: the layout skips it and
	   closes the gap. */
	if (!slot_shown(plugin, sd))
		return 0;
	return gowl_bar_plugin_measure_label(plugin, layout, theme, height);
}

static void
slot_draw(
	GowlBarPlugin      *plugin,
	gpointer            data,
	cairo_t            *cr,
	PangoLayout        *layout,
	const GowlBarTheme *theme,
	gint                x,
	gint                y,
	gint                width,
	gint                height,
	gboolean            hovered,
	gboolean            panel_open
){
	const SlotData *sd = data;

	if (sd->urgency == BAR_SLOT_CRITICAL && width > 2 && height > 4) {
		gint radius;

		radius = gowl_bar_theme_metric(theme, GOWL_BAR_METRIC_RADIUS);
		gowl_bar_cairo_rounded_rect(cr, x + 1, y + 2, width - 2,
		                            height - 4, radius * 0.7);
		gowl_bar_theme_cairo_set_alpha(theme, cr, GOWL_BAR_COLOR_RED,
		                               0.22);
		cairo_fill(cr);
	}

	gowl_bar_plugin_draw_label(plugin, cr, layout, theme, x, y, width,
	                           height, hovered, panel_open);
}

static void
slot_signature(
	GowlBarPlugin *plugin,
	gpointer       data,
	GString       *out
){
	const SlotData *sd = data;

	/* The label, glyph and role are in the common part.  What is left
	   is what only this plugin knows: whether THIS output shows it,
	   and the wash. */
	g_string_append_printf(out, "s%d;u%d;",
	                       slot_shown(plugin, sd) ? 1 : 0,
	                       (gint)sd->urgency);
}

static const gchar *
slot_button_name(
	guint  button,
	gchar *buf,
	gsize  buf_len
){
	if (button == BTN_LEFT)
		return "left";
	if (button == BTN_RIGHT)
		return "right";
	if (button == BTN_MIDDLE)
		return "middle";
	g_snprintf(buf, buf_len, "button-%u", button);
	return buf;
}

static gboolean
slot_click(
	GowlBarPlugin *plugin,
	gpointer       data,
	guint          button,
	gint           x,
	gint           y,
	guint          modifiers
){
	const SlotData *sd = data;
	const BarEnv   *env;
	const gchar    *id;
	const gchar    *owner;
	const gchar    *output;
	gpointer        mon;
	GString        *mods;
	gchar           button_buf[24];

	(void)x;
	(void)y;

	/* A slot nobody asked to hear about is not a button: the click
	   falls through, as it does on a label. */
	if (!sd->click)
		return FALSE;

	env = bar_env();
	if (env == NULL || env->compositor == NULL)
		return FALSE;

	id = gowl_bar_plugin_get_setting(plugin, "param");
	if (id == NULL || id[0] == '\0')
		return FALSE;
	owner = gowl_bar_plugin_get_setting(plugin, "owner");
	if (owner == NULL || owner[0] == '\0')
		owner = "-";

	mon = gowl_bar_plugin_get_monitor(plugin);
	output = (mon != NULL) ? gowl_monitor_get_name(GOWL_MONITOR(mon)) : NULL;
	if (output == NULL || output[0] == '\0')
		output = "-";

	mods = g_string_new(NULL);
	if (modifiers & GOWL_KEY_MOD_SHIFT)
		g_string_append(mods, "shift+");
	if (modifiers & GOWL_KEY_MOD_CTRL)
		g_string_append(mods, "ctrl+");
	if (modifiers & GOWL_KEY_MOD_ALT)
		g_string_append(mods, "alt+");
	if (modifiers & GOWL_KEY_MOD_LOGO)
		g_string_append(mods, "super+");
	if (mods->len > 0)
		g_string_truncate(mods, mods->len - 1);
	else
		g_string_append_c(mods, '-');

	/*
	 * Five words, none of which can hold a space: the id and the owner
	 * are restricted to a safe alphabet when the slot is made, and the
	 * rest are ours.  The text the slot shows is deliberately absent --
	 * the owner knows what it pushed, and an event line is no place for
	 * free text.
	 */
	gowl_compositor_emit_module_event(GOWL_COMPOSITOR(env->compositor),
		"bar", "slot-click %s %s %s %s %s", id,
		slot_button_name(button, button_buf, sizeof(button_buf)),
		mods->str, output, owner);

	g_string_free(mods, TRUE);
	return TRUE;
}

static const GowlBarPluginVTable slot_vtable = {
	sizeof(GowlBarPluginVTable),
	slot_create, slot_destroy,
	NULL, NULL, slot_configure,
	NULL, NULL, NULL,
	slot_measure, slot_draw,
	slot_click, NULL,
	NULL, NULL,
	NULL, NULL,
	NULL,
	slot_signature,
	NULL, NULL
};

/**
 * bar_register_slot_plugins:
 * @registry: the registry to populate
 *
 * Registers `slot', the widget behind every pushed bar item.
 */
void
bar_register_slot_plugins(
	GowlBarRegistry *registry
){
	gowl_bar_registry_register_vtable(registry, "slot", "Slot",
		"A bar item whose content is pushed from outside",
		&slot_vtable);
}
