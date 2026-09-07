/*
 * example-bar-plugin.c - a bar plugin, written as a C script
 *
 * Copyright (C) 2026  Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Drop this file (or a copy of it) into
 *
 *     ~/.config/gowl/bar-plugins/
 *
 * and the bar compiles it on the next start, caches the result under
 * $XDG_CACHE_HOME/gowl/bar-plugins, and registers whatever it
 * publishes.  An unchanged source loads from the cache without
 * invoking the compiler at all.  Nothing has to be installed and no
 * build system is involved: this is the same crispy path gowl's C
 * configuration file uses.
 *
 * Then reference it from the bar's widget list like any built-in:
 *
 *     (gowl-bar-configure
 *       '(("position" . "top")
 *         ("widgets-right" . "pomodoro cpu memory clock")))
 *
 * A precompiled `.so' works too --- same exported symbol, same
 * descriptor --- if you would rather build it yourself.  Reload a
 * changed source without restarting the session with
 *
 *     gowl bar-plugin-reload pomodoro
 *
 * Extra compiler flags, if you need any:
 *
 *     #define CRISPY_PARAMS "$(pkg-config --cflags --libs json-glib-1.0)"
 */

#include <gowl/barkit/gowl-bar-plugin.h>
#include <gowl/barkit/gowl-bar-plugin-proxy.h>
#include <gowl/barkit/gowl-bar-registry.h>

#include <string.h>
#include <time.h>

/* ----------------------------------------------------------------
 * A pomodoro timer
 *
 * Everything a plugin needs is here: per-instance state, a poll that
 * updates the bar text, a click that acts without opening anything,
 * and a panel that acts as the full interface.
 * ---------------------------------------------------------------- */

typedef struct {
	gint64   started_us;     /* 0 while stopped */
	gint     minutes;
	gboolean on_break;
	gint     completed;
} Pomodoro;

/* Called once per instance, before anything else. */
static gpointer
pomodoro_create(GowlBarPlugin *plugin)
{
	Pomodoro *p;

	(void)plugin;
	p = g_new0(Pomodoro, 1);
	p->minutes = 25;
	return p;
}

static void
pomodoro_destroy(GowlBarPlugin *plugin, gpointer data)
{
	(void)plugin;
	g_free(data);
}

/* Settings arrive from the bar's configuration as plain strings.  This
   is called again on every reconfigure, so it must be idempotent. */
static void
pomodoro_configure(GowlBarPlugin *plugin, gpointer data,
                   GHashTable *settings)
{
	Pomodoro *p = data;

	(void)settings;
	p->minutes = gowl_bar_plugin_get_setting_int(plugin, "minutes", 25);
	if (p->minutes < 1)
		p->minutes = 1;
}

/* How often the poll below should run, in seconds. */
static gint
pomodoro_interval(GowlBarPlugin *plugin, gpointer data)
{
	(void)plugin;
	(void)data;
	return 1;
}

/* Seconds left, or -1 when the timer is not running. */
static gint
pomodoro_remaining(Pomodoro *p)
{
	gint64 elapsed;

	if (p->started_us == 0)
		return -1;
	elapsed = (g_get_monotonic_time() - p->started_us) / G_USEC_PER_SEC;
	return (gint)((gint64)p->minutes * 60 - elapsed);
}

/*
 * The poll runs on the compositor's own thread, so it may only do
 * cheap, non-blocking work: arithmetic, a small /proc read.  Anything
 * that spawns a process or touches the network belongs in poll_async
 * instead, which the bar runs on a worker thread.
 */
static void
pomodoro_poll(GowlBarPlugin *plugin, gpointer data)
{
	Pomodoro *p = data;
	gint remaining;
	gchar buf[32];

	remaining = pomodoro_remaining(p);

	if (remaining < 0) {
		gowl_bar_plugin_set_label(plugin, "idle");
		gowl_bar_plugin_set_color(plugin, GOWL_BAR_COLOR_MUTED);
		return;
	}

	if (remaining <= 0) {
		p->started_us = 0;
		if (!p->on_break)
			p->completed++;
		p->on_break = !p->on_break;
		p->minutes = p->on_break ? 5 : 25;

		/* A toast, wired to this plugin's own panel: clicking it
		   opens the timer rather than merely dismissing it. */
		gowl_bar_plugin_notify(plugin, GOWL_BAR_TOAST_NORMAL,
			p->on_break ? "Pomodoro finished"
			            : "Break over",
			p->on_break ? "Take five." : "Back to it.");
		gowl_bar_plugin_set_label(plugin, "done");
		gowl_bar_plugin_set_color(plugin, GOWL_BAR_COLOR_GREEN);
		return;
	}

	g_snprintf(buf, sizeof(buf), "%02d:%02d", remaining / 60,
	           remaining % 60);
	gowl_bar_plugin_set_label(plugin, buf);
	/* Colours are named roles, never hex, so the widget follows
	   whatever palette the session is using. */
	gowl_bar_plugin_set_color(plugin,
		p->on_break ? GOWL_BAR_COLOR_TEAL
		: (remaining < 60) ? GOWL_BAR_COLOR_PEACH
		: GOWL_BAR_COLOR_TEXT);
}

/* Return TRUE to consume the click; returning FALSE lets the bar fall
   back to opening this plugin's panel. */
static gboolean
pomodoro_click(GowlBarPlugin *plugin, gpointer data, guint button, gint x,
               gint y, guint modifiers)
{
	Pomodoro *p = data;

	(void)plugin;
	(void)x;
	(void)y;
	(void)modifiers;

	/* Middle-click starts and stops without opening anything. */
	if (button == 0x112 /* BTN_MIDDLE */) {
		p->started_us = (p->started_us == 0)
			? g_get_monotonic_time() : 0;
		return TRUE;
	}
	return FALSE;
}

/*
 * The panel is described, not drawn: the bar renders it, hit-tests it,
 * scrolls it and gives it keyboard navigation.  That is what makes a
 * plugin from outside the tree look and behave exactly like a shipped
 * one.
 */
static GowlBarPanel *
pomodoro_panel(GowlBarPlugin *plugin, gpointer data)
{
	Pomodoro *p = data;
	GowlBarPanel *panel;
	GowlBarPanelItem *item;
	gint remaining;
	gchar buf[64];

	(void)plugin;

	panel = gowl_bar_panel_new();
	gowl_bar_panel_set_width(panel, 380);

	remaining = pomodoro_remaining(p);
	if (remaining > 0) {
		g_snprintf(buf, sizeof(buf), "%02d:%02d", remaining / 60,
		           remaining % 60);
	} else {
		g_strlcpy(buf, "Ready", sizeof(buf));
	}

	item = gowl_bar_panel_add_hero(panel, "\xef\x80\x97", buf,
		p->on_break ? "On a break" : "Focus");
	/* Giving the hero an id turns its trailing switch on. */
	gowl_bar_panel_item_set_id(item, "running");
	gowl_bar_panel_item_set_active(item, p->started_us != 0);

	if (remaining > 0) {
		item = gowl_bar_panel_add_progress(panel, "Elapsed",
			1.0 - (gdouble)remaining /
			      ((gdouble)p->minutes * 60.0));
		gowl_bar_panel_item_set_color(item,
			p->on_break ? GOWL_BAR_COLOR_TEAL
			            : GOWL_BAR_COLOR_ACCENT);
	}

	g_snprintf(buf, sizeof(buf), "%d", p->completed);
	gowl_bar_panel_add_field_pair(panel, "Completed", buf, "Length",
		p->on_break ? "5 min" : "25 min");

	gowl_bar_panel_add_separator(panel);
	item = gowl_bar_panel_add_buttons(panel, "action");
	gowl_bar_panel_add_button(item,
		(p->started_us != 0) ? "Stop" : "Start", FALSE);
	gowl_bar_panel_add_button(item, "Reset", FALSE);
	gowl_bar_panel_add_button(item, "Break", p->on_break);

	return panel;
}

/*
 * A panel item was activated.  @item_id is the id the item was built
 * with and @index is the button's position within a button row (or -1);
 * @value carries a slider's new position.
 */
static void
pomodoro_action(GowlBarPlugin *plugin, gpointer data, const gchar *item_id,
                gint index, gdouble value, guint button)
{
	Pomodoro *p = data;

	(void)plugin;
	(void)value;
	(void)button;

	if (g_strcmp0(item_id, "running") == 0) {
		p->started_us = (p->started_us == 0)
			? g_get_monotonic_time() : 0;
		return;
	}

	if (g_strcmp0(item_id, "action") != 0)
		return;

	switch (index) {
	case 0:
		p->started_us = (p->started_us == 0)
			? g_get_monotonic_time() : 0;
		break;
	case 1:
		p->started_us = 0;
		p->on_break = FALSE;
		p->minutes = gowl_bar_plugin_get_setting_int(plugin, "minutes",
		                                             25);
		break;
	case 2:
		p->on_break = TRUE;
		p->minutes = 5;
		p->started_us = g_get_monotonic_time();
		break;
	default:
		break;
	}
}

/*
 * The vtable.  Leave a slot NULL and the bar uses its default: the
 * standard label drawing, no panel, no click handling.  `size' must be
 * the first member and must be sizeof the struct as this file sees it
 * --- that is what lets the bar grow the vtable without breaking a
 * plugin built against an older gowl.
 */
static const GowlBarPluginVTable pomodoro_vtable = {
	sizeof(GowlBarPluginVTable),
	pomodoro_create,        /* create */
	pomodoro_destroy,       /* destroy */
	NULL,                   /* activate */
	NULL,                   /* deactivate */
	pomodoro_configure,     /* configure */
	pomodoro_interval,      /* interval */
	pomodoro_poll,          /* poll (compositor thread) */
	NULL,                   /* poll_async (worker thread) */
	NULL,                   /* measure */
	NULL,                   /* draw */
	pomodoro_click,         /* click */
	NULL,                   /* scroll */
	pomodoro_panel,         /* panel */
	pomodoro_action,        /* action */
	NULL,                   /* panel_opened */
	NULL                    /* panel_closed */
};

/* One file may publish several plugins; they are then one compile and
   one reload rather than several. */
static const GowlBarPluginDesc descs[] = {
	{
		GOWL_BAR_PLUGIN_ABI,
		"pomodoro",
		"Pomodoro",
		"A focus timer with a panel",
		"1.0.0",
		&pomodoro_vtable,
		NULL,
		{ NULL, NULL, NULL, NULL }
	}
};

/*
 * The one symbol the bar looks for.  Everything it returns must stay
 * valid for as long as the file is loaded, so it is static const.
 */
G_MODULE_EXPORT const GowlBarPluginDesc *
gowl_bar_plugin_query(guint *n_descs)
{
	*n_descs = G_N_ELEMENTS(descs);
	return descs;
}
