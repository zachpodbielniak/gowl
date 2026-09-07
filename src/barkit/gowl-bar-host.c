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

#include "barkit/gowl-bar-host.h"

G_DEFINE_INTERFACE(GowlBarHost, gowl_bar_host, G_TYPE_OBJECT)

static void
gowl_bar_host_default_init(GowlBarHostInterface *iface)
{
	(void)iface;
}

/**
 * gowl_bar_host_get_theme:
 * @self: a host
 *
 * Returns: (transfer none) (nullable): the active theme
 */
const GowlBarTheme *
gowl_bar_host_get_theme(GowlBarHost *self)
{
	GowlBarHostInterface *iface;

	g_return_val_if_fail(GOWL_IS_BAR_HOST(self), NULL);

	iface = GOWL_BAR_HOST_GET_IFACE(self);
	if (iface->get_theme == NULL)
		return NULL;
	return iface->get_theme(self);
}

gboolean
gowl_bar_host_set_bar_setting(GowlBarHost *self, const gchar *key,
                              const gchar *value)
{
	GowlBarHostInterface *iface;

	g_return_val_if_fail(GOWL_IS_BAR_HOST(self), FALSE);
	g_return_val_if_fail(key != NULL, FALSE);

	iface = GOWL_BAR_HOST_GET_IFACE(self);
	if (iface->set_bar_setting == NULL)
		return FALSE;
	return iface->set_bar_setting(self, key, value);
}

/**
 * gowl_bar_host_request_redraw:
 * @self: a host
 */
void
gowl_bar_host_request_redraw(GowlBarHost *self)
{
	GowlBarHostInterface *iface;

	g_return_if_fail(GOWL_IS_BAR_HOST(self));

	iface = GOWL_BAR_HOST_GET_IFACE(self);
	if (iface->request_redraw != NULL)
		iface->request_redraw(self);
}

/**
 * gowl_bar_host_request_panel_refresh:
 * @self: a host
 * @plugin: the plugin whose panel changed
 */
void
gowl_bar_host_request_panel_refresh(GowlBarHost *self, GowlBarPlugin *plugin)
{
	GowlBarHostInterface *iface;

	g_return_if_fail(GOWL_IS_BAR_HOST(self));

	iface = GOWL_BAR_HOST_GET_IFACE(self);
	if (iface->request_panel_refresh != NULL)
		iface->request_panel_refresh(self, plugin);
}

/**
 * gowl_bar_host_open_panel:
 * @self: a host
 * @plugin_id: the plugin whose panel to open
 */
void
gowl_bar_host_open_panel(GowlBarHost *self, const gchar *plugin_id)
{
	GowlBarHostInterface *iface;

	g_return_if_fail(GOWL_IS_BAR_HOST(self));

	iface = GOWL_BAR_HOST_GET_IFACE(self);
	if (iface->open_panel != NULL)
		iface->open_panel(self, plugin_id);
}

/**
 * gowl_bar_host_close_panel:
 * @self: a host
 */
void
gowl_bar_host_close_panel(GowlBarHost *self)
{
	GowlBarHostInterface *iface;

	g_return_if_fail(GOWL_IS_BAR_HOST(self));

	iface = GOWL_BAR_HOST_GET_IFACE(self);
	if (iface->close_panel != NULL)
		iface->close_panel(self);
}

/**
 * gowl_bar_host_notify:
 * @self: a host
 * @toast: (transfer full): the toast to show
 */
void
gowl_bar_host_notify(GowlBarHost *self, GowlBarToast *toast)
{
	GowlBarHostInterface *iface;

	if (toast == NULL)
		return;
	if (!GOWL_IS_BAR_HOST(self)) {
		gowl_bar_toast_free(toast);
		g_return_if_fail(GOWL_IS_BAR_HOST(self));
		return;
	}

	iface = GOWL_BAR_HOST_GET_IFACE(self);
	if (iface->notify == NULL) {
		gowl_bar_toast_free(toast);
		return;
	}
	iface->notify(self, toast);
}

/**
 * gowl_bar_host_queue_work:
 * @self: a host
 * @plugin: the owning plugin
 * @func: the blocking callback
 * @user_data: passed to @func
 * @destroy: (nullable): frees @user_data afterwards
 */
void
gowl_bar_host_queue_work(GowlBarHost *self, GowlBarPlugin *plugin,
                         GowlBarWorkFunc func, gpointer user_data,
                         GDestroyNotify destroy)
{
	GowlBarHostInterface *iface;

	if (func == NULL) {
		if (destroy != NULL && user_data != NULL)
			destroy(user_data);
		return;
	}
	if (!GOWL_IS_BAR_HOST(self)) {
		if (destroy != NULL && user_data != NULL)
			destroy(user_data);
		return;
	}

	iface = GOWL_BAR_HOST_GET_IFACE(self);
	if (iface->queue_work == NULL) {
		if (destroy != NULL && user_data != NULL)
			destroy(user_data);
		return;
	}
	iface->queue_work(self, plugin, func, user_data, destroy);
}

/**
 * gowl_bar_host_spawn:
 * @self: a host
 * @cmdline: a shell command line
 */
void
gowl_bar_host_spawn(GowlBarHost *self, const gchar *cmdline)
{
	GowlBarHostInterface *iface;

	g_return_if_fail(GOWL_IS_BAR_HOST(self));

	if (cmdline == NULL || cmdline[0] == '\0')
		return;

	iface = GOWL_BAR_HOST_GET_IFACE(self);
	if (iface->spawn != NULL)
		iface->spawn(self, cmdline);
}

/**
 * gowl_bar_host_get_state_dir:
 * @self: a host
 *
 * Returns: (transfer none) (nullable): a writable directory for plugin
 *   state, or %NULL when the host offers none
 */
const gchar *
gowl_bar_host_get_state_dir(GowlBarHost *self)
{
	GowlBarHostInterface *iface;

	g_return_val_if_fail(GOWL_IS_BAR_HOST(self), NULL);

	iface = GOWL_BAR_HOST_GET_IFACE(self);
	if (iface->get_state_dir == NULL)
		return NULL;
	return iface->get_state_dir(self);
}
